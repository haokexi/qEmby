find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(qemby_layout_feedback_test
    layoutfeedbacktest.cpp
    ../src/qEmbyApp/components/detailbottominfowidget.cpp
    ../src/qEmbyApp/components/detailbottominfowidget.h
    ../src/qEmbyApp/components/detailtagbutton.cpp
    ../src/qEmbyApp/components/detailtagbutton.h
    ../src/qEmbyApp/components/flowlayout.cpp
    ../src/qEmbyApp/components/flowlayout.h
    ../src/qEmbyApp/components/horizontalwidgetgallery.cpp
    ../src/qEmbyApp/components/horizontalwidgetgallery.h
)

target_include_directories(qemby_layout_feedback_test PRIVATE
    ../src/qEmbyApp
    ../src/qEmbyCore
)

target_link_libraries(qemby_layout_feedback_test PRIVATE
    Qt6::Test
    Qt6::Widgets
    qEmbyCore
)

add_test(NAME qemby_layout_feedback_test
         COMMAND qemby_layout_feedback_test)
set_tests_properties(qemby_layout_feedback_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
