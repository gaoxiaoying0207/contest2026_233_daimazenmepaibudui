# app/robot_ui/CMakeLists.txt

# 条件编译：启用 ZHI_AI 或 ROBOT_UI 时编译
if(CONFIG_LVX_USE_CONTEST2026_233_ZHI_AI OR CONFIG_LVX_USE_CONTEST2026_233_ROBOT_UI)

  # hello_app 头文件路径 (AI 模块接口)
  set(ROBOT_UI_COMPILE_FLAGS
    -I${CMAKE_CURRENT_SOURCE_DIR}/../hello_app
  )

  # 包含 hello_app 源文件 (AI 模块实现)
  set(ROBOT_UI_SRCS
    main.c
    robot_ui.c
    touch_ui.c
    network_comm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app/ai_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app/ai_audio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app/ai_llm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app/ai_sound_detect.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../hello_app/ai_care.c
  )

  # 添加应用程序
  nuttx_add_application(
    NAME robot_ui
    SRCS ${ROBOT_UI_SRCS}
    STACKSIZE ${CONFIG_DEFAULT_TASK_STACKSIZE}
    PRIORITY 100
    COMPILE_FLAGS ${ROBOT_UI_COMPILE_FLAGS}
  )

endif()
