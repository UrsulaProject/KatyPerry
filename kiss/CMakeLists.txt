cmake_minimum_required(VERSION 3.10)
project(KISS VERSION 0.0.1)
am_init("Naville")
am_add_tweak(NAME KISS SRCS "${CMAKE_CURRENT_LIST_DIR}/Tweak.xm" NOSUBSTRATE)
find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
find_library(UIKIT_LIBRARY UIKit REQUIRED)
find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
target_link_libraries(KISS PUBLIC
    ${FOUNDATION_LIBRARY}
    ${UIKIT_LIBRARY}
    ${QUARTZCORE_LIBRARY}
)
