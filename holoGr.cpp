#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <map>
#include <vector>
#include <filesystem>
#include <chrono>
#include <deque>
#include <numeric>
#include <unordered_set>
#include <queue>
#include <mutex>
#include <condition_variable>




struct TextureUploadRequest {
    int angle;
    int width, height, channels;
    std::vector<unsigned char> image_data;
};

std::queue<TextureUploadRequest> upload_queue;
std::mutex upload_mutex;

std::queue<int> load_queue;
std::mutex load_mutex;
std::condition_variable load_cv;

std::unordered_set<int> pending_textures;
std::mutex pending_mutex;

std::thread loader_thread;

namespace fs = std::filesystem;

// === Constants ===
const int WINDOW_WIDTH  = 1280;
const int WINDOW_HEIGHT = 720;
const int TOTAL_IMAGES  = 180;
const int BUFFER_SIZE   = 10; // number of images to buffer before/after current
const float MAX_FPS     = 120.;
const float FRAME_TIME  = 1./MAX_FPS;

std::atomic<bool> running = true;
std::atomic<bool> sync_flag{true};
bool show_left_eye = true;
int current_angle  = 0;
int dirchange      = 0;
int FoVchange      = 0;
int eye_separation = 1;

using Clock = std::chrono::high_resolution_clock;
using TimePoint = std::chrono::time_point<Clock>;
std::deque<double> frame_times;  // Store last N frame durations
const size_t MAX_SAMPLES = 10;  // ~1 second window at 120Hz

// === Image Cache ===
std::map<int, GLuint> texture_cache;

// === Path Settings ===
std::string image_dir = "images";

int serial_fd = -1;

bool initSerial(const char* port) {
    serial_fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (serial_fd < 0) return false;
    //std::cout << serial_fd;

    struct termios tty;
    if (tcgetattr(serial_fd, &tty) != 0) return false;

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 5;

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    return tcsetattr(serial_fd, TCSANOW, &tty) == 0;
}

void sendEyeSignal(int fd, bool left_eye) {
    int status;
    ioctl(fd, TIOCMGET, &status);
    if (left_eye)
        status |= TIOCM_RTS;
    else
        status &= ~TIOCM_RTS;
    ioctl(fd, TIOCMSET, &status);
}


void logFrameTiming(double dt_ms) {
    frame_times.push_back(dt_ms);
    if (frame_times.size() > MAX_SAMPLES)
        frame_times.pop_front();

    if (frame_times.size() == MAX_SAMPLES) {
        double avg = std::accumulate(frame_times.begin(), frame_times.end(), 0.0) / frame_times.size();
        double sq_sum = 0.0;
        for (auto f : frame_times)
            sq_sum += (f - avg) * (f - avg);
        double stddev = std::sqrt(sq_sum / frame_times.size());

        std::cout << "[Frame] Avg: " << avg << "ms | StdDev: " << stddev << "ms" << std::endl;
        frame_times.clear();
    }
}

// === Load/Compile Shaders ===
std::string loadShaderSource(const char* filePath) {
    std::ifstream file(filePath);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

GLuint compileShader(const char* path, GLenum type) {
    std::string code = loadShaderSource(path);
    const char* src = code.c_str();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[512];
        glGetShaderInfoLog(shader, 512, nullptr, info);
        std::cerr << "Shader error in " << path << ":\n" << info << std::endl;
    }
    return shader;
}

GLuint createShaderProgram() {
    GLuint vertex = compileShader("shaders/vertex.glsl", GL_VERTEX_SHADER);
    GLuint fragment = compileShader("shaders/fragment.glsl", GL_FRAGMENT_SHADER);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return program;
}

// === Texture Management ===
GLuint loadTextureFromFile(const std::string& path) {
    int w, h, channels;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!data) return 0;

    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(data);
    return texID;
}

GLuint generatePlaceholderTexture(float r, float g, float b) {
    unsigned char data[3] = {
        static_cast<unsigned char>(r * 255),
        static_cast<unsigned char>(g * 255),
        static_cast<unsigned char>(b * 255)
    };
    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    return texID;
}

int wrapIndex(int angle) {
    return (angle + TOTAL_IMAGES) % TOTAL_IMAGES;
}

std::string angleFilename(int angle) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s/sar_%04d.png", image_dir.c_str(), wrapIndex(angle)+199);
    return std::string(buf);
}

GLuint getTextureForAngle(int angle) {
    int idx = wrapIndex(angle);
    if (texture_cache.count(idx)) return texture_cache[idx];

    std::string path = angleFilename(idx);
    GLuint tex = loadTextureFromFile(path);
    if (tex == 0) {
        tex = generatePlaceholderTexture(0.2f, 0.2f, 0.2f); // fallback gray
    }
    texture_cache[idx] = tex;
    return tex;
}

void preloadTexturesAroundAngles(const std::vector<int>& angles) {
    std::unordered_set<int> to_load;
    {
	    std::lock_guard<std::mutex> lock(pending_mutex);
	    for (int center : angles) {
	        for (int offset = -BUFFER_SIZE; offset <= BUFFER_SIZE; ++offset) {
	            int index = wrapIndex(center + offset);
	            if (texture_cache.find(index) == texture_cache.end() &&
	                pending_textures.find(index) == pending_textures.end()) {
	            	//std::cout << index<<std::endl;
	            	//std::cout.flush();
	                to_load.insert(index);
	                pending_textures.insert(index);  // mark as pending
	            }
	        }
	    }
	}
    std::lock_guard<std::mutex> lock(load_mutex);
    for (int idx : to_load) {
        load_queue.push(idx);
    }
    load_cv.notify_all();
}

void clearOldTexturesAroundAngles(const std::vector<int>& angles) {
    std::unordered_set<int> keep;
    for (int center : angles) {
        for (int offset = -BUFFER_SIZE; offset <= BUFFER_SIZE; offset++) {
            keep.insert(wrapIndex(center + offset));
        }
    }

	for (auto it = texture_cache.begin(); it != texture_cache.end(); ) {
    	if (keep.find(it->first) == keep.end()) {
        	glDeleteTextures(1, &it->second);
        	it = texture_cache.erase(it);  // erase returns the next valid iterator
    	} else {
    	    ++it;
    	}
	}
}

void processUploads() {
    std::unique_lock<std::mutex> lock(upload_mutex);
    while (!upload_queue.empty()) {
        TextureUploadRequest req = std::move(upload_queue.front());
        upload_queue.pop();
        lock.unlock();
        //std::cout<<req.angle<<std::endl;
        //std::cout.flush();

        GLuint tex_id;
        glGenTextures(1, &tex_id);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexImage2D(GL_TEXTURE_2D, 0,
                     req.channels == 4 ? GL_RGBA : GL_RGB,
                     req.width, req.height, 0,
                     req.channels == 4 ? GL_RGBA : GL_RGB,
                     GL_UNSIGNED_BYTE, req.image_data.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        texture_cache[req.angle] = tex_id;

        lock.lock();
        std::lock_guard<std::mutex> pending_lock(pending_mutex);
		pending_textures.erase(req.angle);
    }
}

// === Input ===
void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action == GLFW_PRESS) {
        switch (key) {
            case GLFW_KEY_LEFT:
            	dirchange =  1;
                //current_angle = wrapIndex(current_angle - 1);
                break;
            case GLFW_KEY_RIGHT:
            	dirchange = -1;
                //current_angle = wrapIndex(current_angle + 1);
                break;
            case GLFW_KEY_UP:
            	FoVchange = 1;
                //eye_separation += 2;
                break;
            case GLFW_KEY_DOWN:
            	FoVchange= -1;
                //eye_separation = std::max(0, eye_separation - 2);
                break;
            case GLFW_KEY_A:
                glfwSetWindowShouldClose(window, GLFW_TRUE);
                break;
        }
    }
}

void update_and_swap(GLFWwindow* window,int dir, int FoV){

	current_angle  = wrapIndex(current_angle + dir);
	eye_separation = std::max(0, eye_separation + FoV);
    int leftIndex  = wrapIndex(current_angle - eye_separation);
    int rightIndex = wrapIndex(current_angle + eye_separation);
    //std::cout << rightIndex << " " << leftIndex << " " << dir << std::endl;
    //std::cout.flush();

    GLuint tex = show_left_eye ? getTextureForAngle(leftIndex) : getTextureForAngle(rightIndex);
    
    //std::cout<<leftIndex<<" "<rightIndex<<std::endl;
    //std::cout.flush();

    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    show_left_eye = !show_left_eye;
    
    std::vector<int> eye_angles = { leftIndex, rightIndex };

    preloadTexturesAroundAngles(eye_angles);
    clearOldTexturesAroundAngles(eye_angles);
        
}

// === Sync Thread ===
void syncLoop() {
    using namespace std::chrono;
    const auto interval = microseconds(1000); // ~120Hz

    while (running) {
    	if (!sync_flag.load()){
        	std::this_thread::sleep_for(interval);
        	sync_flag.store(true); 
        	glfwPostEmptyEvent(); // wake up main thread
        }
    }
}

void loaderLoop() {
    while (running) {
        int angle;
        {
            std::unique_lock<std::mutex> lock(load_mutex);
            load_cv.wait(lock, [] { return !load_queue.empty() || !running; });
            if (!running) break;
            angle = load_queue.front();
            load_queue.pop();
        }

        std::string path = angleFilename(angle);
        int w, h, ch;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 0);
        if (!data) continue;
        TextureUploadRequest req;
        req.angle = angle;
        req.width = w;
        req.height = h;
        req.channels = ch;
        req.image_data.assign(data, data + w * h * ch);
        stbi_image_free(data);

        {
            std::lock_guard<std::mutex> lock(upload_mutex);
            upload_queue.push(std::move(req));
        }
    }
}

// === Main ===
int main(int argc, char** argv) {

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <image_directory>\n";
        return 1;
    }

    image_dir = argv[1];

    if (!fs::is_directory(image_dir)) {
        std::cerr << "Error: '" << image_dir << "' is not a valid directory.\n";
        return 1;
    }
    
    glfwInit();
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    
    glfwWindowHint(GLFW_REFRESH_RATE, MAX_FPS);
    glfwWindowHint(GLFW_RED_BITS, mode->redBits);
    glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
    glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
    
    GLFWwindow* window = glfwCreateWindow(mode->width,mode->height, "Stereo Viewer",  monitor, nullptr);
    glfwMakeContextCurrent(window);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
    glfwSetKeyCallback(window, keyCallback);

    GLuint shader = createShaderProgram();
    glUseProgram(shader);

    float vertices[] = {
        -1, -1, 0, 0,
         1, -1, 1, 0,
         1,  1, 1, 1,
        -1,  1, 0, 1
    };
    unsigned int indices[] = { 0, 1, 2, 2, 3, 0 };

    GLuint VAO, VBO, EBO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);
    glGenBuffers(1, &VBO); glGenBuffers(1, &EBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    GLint posLoc = glGetAttribLocation(shader, "aPos");
    GLint texLoc = glGetAttribLocation(shader, "aTexCoord");
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(texLoc);

    GLint texUniform = glGetUniformLocation(shader, "texture1");
    glUniform1i(texUniform, 0);
    
    initSerial("/dev/ttyUSB0");
    
    glfwSwapInterval(1);

    //std::thread syncThread(syncLoop);
    
    std::thread loader_thread(loaderLoop);
    float current_time = glfwGetTime();
    float last_time = current_time;
    float elapsed_time = 0.f;

    while (!glfwWindowShouldClose(window)) {
        processUploads();
        glfwPollEvents();
        current_time = glfwGetTime();
        float dt = current_time - last_time;
        last_time = current_time;
        elapsed_time += dt;
    	if (sync_flag.load()) {
    		//processUploads();
    		if (show_left_eye && dirchange>0) {
				update_and_swap(window,dirchange,0);
				dirchange = 0;
    		}
    		else if (!show_left_eye && dirchange<0) {
    			update_and_swap(window,dirchange,0);
				dirchange = 0;
    		}
    		else if (FoVchange !=0){
    			update_and_swap(window,0,FoVchange);
    			FoVchange = 0;
    		}
    		else{
    			update_and_swap(window,0,0);
    		}
            sync_flag.store(false);

            }
        if (elapsed_time >= FRAME_TIME){
    		TimePoint t_start = Clock::now();
            sendEyeSignal(serial_fd,show_left_eye);
            glfwSwapBuffers(window);
            // GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    		// GLenum result = glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000000);
            TimePoint t_end = Clock::now();
    		double dt_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
            //logFrameTiming(dt_ms);
            // if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) {

            // }
            std::cout << elapsed_time << " VSync pulse (frame complete)\n";
            sync_flag.store(true);
            elapsed_time = 0.f;
            //glDeleteSync(sync);
        }
    }

    running = false;
    //syncThread.join();
    load_cv.notify_all();
    loader_thread.join();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
    if (serial_fd >= 0) close(serial_fd);
}
