cmake_minimum_required(VERSION 3.0)

project(stereoViewer VERSION 1.0.1 LANGUAGES CXX )

# cmake -DCMAKE_BUILD_TYPE=Debug
message ("build type : " ${CMAKE_BUILD_TYPE})
# ajout d'un suffix dans le nom des bibliotheques en debug (pas des executables)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set (CMAKE_DEBUG_POSTFIX "D")
endif()

# find_package(OpenMP)
# if (OPENMP_FOUND)
#     set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
#     set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
# endif()

# optim option : -O3 -march=corei7 -mavx2 -msse4.2 -mfma -ffast-math
# for std::tuple : -std=c++11
# -std=c++17
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -mavx2 -mfma -ffast-math -std=c++17")
message ("operating system option : " ${CMAKE_CXX_FLAGS})

# include_directories("${CMAKE_CURRENT_SOURCE_DIR}/glad/include/")
include_directories("/usr/include/glad")
include_directories("/usr/include/stb")
# link_directories(/home/tivporta13/regen/fmodstudioapi20002linux/api/core/lib/x86_64/)

set(common_sources
./glad/src/glad.cpp
# glad.cpp
)

set(stereoViewer_sources
${common_sources}
holoGr.cpp
)


# build executable
#set (OUT_EXE_NAME audioFootSwitch${CMAKE_DEBUG_POSTFIX})
#add_executable (${OUT_EXE_NAME} main_cpp.cpp ${audioFootSwitch_sources} ${headers})

# # build librairie
# set (OUT_LIB_NAME AudioFootSwitchLib${CMAKE_DEBUG_POSTFIX})
# add_library(${OUT_LIB_NAME} SHARED entrylib.cpp ${audioFootSwitch_sources} ${headers})

# build executable
set (OUT_EXE_NAME stereoViewer${CMAKE_DEBUG_POSTFIX})
add_executable (${OUT_EXE_NAME} ${stereoViewer_sources})


# set_target_properties(${OUT_LIB_NAME} PROPERTIES VERSION ${PROJECT_VERSION})
# set_target_properties(${OUT_LIB_NAME} PROPERTIES SOVERSION 1)

target_link_libraries(${OUT_EXE_NAME}
pthread
glfw
dl
GL
)
