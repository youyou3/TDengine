CMAKE_MINIMUM_REQUIRED(VERSION 2.8)
PROJECT(TDengine)

INCLUDE_DIRECTORIES(${TD_COMMUNITY_DIR}/src/inc)
INCLUDE_DIRECTORIES(${TD_COMMUNITY_DIR}/src/client/inc)
INCLUDE_DIRECTORIES(${TD_OS_DIR}/inc)
INCLUDE_DIRECTORIES(inc)

IF ((TD_LINUX_64) OR (TD_LINUX_32 AND TD_ARM))
  AUX_SOURCE_DIRECTORY(./src SRC)
  LIST(REMOVE_ITEM SRC ./src/shellWindows.c)
  LIST(REMOVE_ITEM SRC ./src/shellDarwin.c)
  ADD_EXECUTABLE(shell ${SRC})  
  
  IF (TD_PAGMODE_LITE)
    TARGET_LINK_LIBRARIES(shell taos)
  ELSE ()
    TARGET_LINK_LIBRARIES(shell taos_static)
  ENDIF ()
  
  SET_TARGET_PROPERTIES(shell PROPERTIES OUTPUT_NAME taos)
ELSEIF (TD_WINDOWS_64 OR TD_WINDOWS_32)
  INCLUDE_DIRECTORIES(${TD_COMMUNITY_DIR}/deps/pthread)
  INCLUDE_DIRECTORIES(${TD_COMMUNITY_DIR}/deps/regex)
  LIST(APPEND SRC ./src/shellEngine.c)
  LIST(APPEND SRC ./src/shellMain.c)
  LIST(APPEND SRC ./src/shellWindows.c)
  ADD_EXECUTABLE(shell ${SRC})
  TARGET_LINK_LIBRARIES(shell taos_static)
  SET_TARGET_PROPERTIES(shell PROPERTIES OUTPUT_NAME taos)
ELSEIF (TD_DARWIN_64)
  LIST(APPEND SRC ./src/shellEngine.c)
  LIST(APPEND SRC ./src/shellMain.c)
  LIST(APPEND SRC ./src/shellDarwin.c)
  LIST(APPEND SRC ./src/shellCommand.c)
  LIST(APPEND SRC ./src/shellImport.c)
  ADD_EXECUTABLE(shell ${SRC})
  TARGET_LINK_LIBRARIES(shell taos_static)
  SET_TARGET_PROPERTIES(shell PROPERTIES OUTPUT_NAME taos)
ENDIF ()

