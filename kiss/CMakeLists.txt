cmake_minimum_required(VERSION 3.10)
project(KISS VERSION 0.0.1)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/modules/")
include(GetGitRevisionDescription)
# Ensure Git exists to properly detect refspec
find_package(Git REQUIRED)
get_git_head_revision(GIT_REFSPEC GIT_COMMIT_HASH
                      ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR)
# Extract branch name from refspec
string(REGEX MATCH "refs/heads/" GIT_REFSPEC_VALID "${GIT_REFSPEC}")
if(GIT_REFSPEC_VALID)
  string(REGEX REPLACE "refs/heads/" "" GIT_REFSPEC "${GIT_REFSPEC}")
endif()
message(STATUS "[KISS] Git Branch : ${GIT_REFSPEC}")
message(STATUS "[KISS] Git SHA1 : ${GIT_COMMIT_HASH}")
am_init("Naville")
am_add_tweak(NAME KISS SRCS "${CMAKE_CURRENT_LIST_DIR}/Tweak.xm" "${CMAKE_CURRENT_LIST_DIR}/fishhook.c" NOSUBSTRATE)
find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
find_library(UIKIT_LIBRARY UIKit REQUIRED)
find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
target_include_directories(KISS PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(KISS PUBLIC
    ${FOUNDATION_LIBRARY}
    ${UIKIT_LIBRARY}
    ${QUARTZCORE_LIBRARY}
)
target_compile_definitions(KISS PRIVATE GIT_REFSPEC="${GIT_REFSPEC}" GIT_COMMIT_HASH="${GIT_COMMIT_HASH}")
