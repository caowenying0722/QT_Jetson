cmake_minimum_required(VERSION 3.16)
project(SmartLockDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc imgcodecs dnn videoio)
find_package(Qt5 5.12 REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)

add_library(smartlock_core src/face_service.cpp)
target_include_directories(smartlock_core PUBLIC src ${OpenCV_INCLUDE_DIRS})
target_link_libraries(smartlock_core PUBLIC Qt5::Core ${OpenCV_LIBS})

configure_file(tools/fingerprint_uart.py fingerprint_uart.py COPYONLY)
configure_file(tools/gesture_paj7620.py gesture_paj7620.py COPYONLY)
add_library(smartlock_window src/demo_window.cpp src/camera_session.cpp src/live_decision.cpp
    src/fingerprint_dialog.cpp src/gesture_dialog.cpp src/unlock_history.cpp src/pwm_servo.cpp
    src/device_check.cpp src/latch_feedback.cpp)
set_target_properties(smartlock_window PROPERTIES AUTOMOC ON)
target_link_libraries(smartlock_window PUBLIC smartlock_core Qt5::Widgets Threads::Threads)

add_executable(smartlock src/main.cpp)
target_link_libraries(smartlock PRIVATE smartlock_core)

add_executable(smartlock_gui src/gui_main.cpp)
target_link_libraries(smartlock_gui PRIVATE smartlock_window)

include(CTest)
if(BUILD_TESTING)
    add_executable(face_gesture_challenge_test tests/face_gesture_challenge_test.cpp)
    target_include_directories(face_gesture_challenge_test PRIVATE src)
    add_test(NAME face_gesture_challenge COMMAND face_gesture_challenge_test)
    add_executable(live_decision_test tests/live_decision_test.cpp)
    target_link_libraries(live_decision_test PRIVATE smartlock_window)
    add_test(NAME live_decision COMMAND live_decision_test)
    add_executable(gui_smoke tests/gui_smoke.cpp)
    target_link_libraries(gui_smoke PRIVATE smartlock_window)
    add_test(NAME gui_smoke COMMAND gui_smoke "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set_tests_properties(gui_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME fingerprint_uart COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/fingerprint_uart_test.py"
        "${CMAKE_CURRENT_BINARY_DIR}/fingerprint_uart.py")
    add_test(NAME gesture_sequence COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/gesture_sequence_test.py"
        "${CMAKE_CURRENT_BINARY_DIR}/gesture_paj7620.py")
    configure_file(tests/gesture_helper_fixture.py gesture-test/gesture_paj7620.py COPYONLY)
    add_executable(gesture_gui_test tests/gesture_gui_test.cpp)
    target_link_libraries(gesture_gui_test PRIVATE smartlock_window)
    set_target_properties(gesture_gui_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gesture-test")
    add_test(NAME gesture_gui COMMAND gesture_gui_test "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set_tests_properties(gesture_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
    add_test(NAME servo_pwm COMMAND gesture_gui_test "${CMAKE_CURRENT_SOURCE_DIR}/assets" --pwm)
    set_tests_properties(servo_pwm PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
    add_executable(fingerprint_gui_test tests/fingerprint_gui_test.cpp)
    target_link_libraries(fingerprint_gui_test PRIVATE smartlock_window)
    add_test(NAME fingerprint_gui COMMAND fingerprint_gui_test "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set_tests_properties(fingerprint_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_test(NAME cli_flow COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_flow.sh"
        "$<TARGET_FILE:smartlock>" "${CMAKE_CURRENT_SOURCE_DIR}/assets")
endif()
