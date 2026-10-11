cmake_minimum_required(VERSION 3.20)
project(SoundPad LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# miniaudio: воспроизведение + декодеры WAV/MP3/FLAC, OGG Vorbis через stb_vorbis.
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.21
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake_)
# SQLite amalgamation.
FetchContent_Declare(sqlite
    GIT_REPOSITORY https://github.com/rhuijben/sqlite-amalgamation.git
    GIT_TAG        3.50.4
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_MakeAvailable(miniaudio sqlite)

add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DEFAULT_FOREIGN_KEYS=1)

add_executable(soundpad
    src/main.cpp
    src/App.cpp
    src/AudioEngine.cpp
    src/AudioFormats.cpp
    src/Config.cpp
    src/Console.cpp
    src/Database.cpp
    src/Hotkey.cpp
    src/miniaudio_impl.cpp)

if(WIN32)
    target_sources(soundpad PRIVATE src/HotkeyListener_win.cpp)
    target_compile_definitions(soundpad PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(soundpad PRIVATE user32 ole32 shell32)
else()
    target_sources(soundpad PRIVATE src/HotkeyListener_stub.cpp)
endif()

target_include_directories(soundpad PRIVATE src ${miniaudio_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(soundpad PRIVATE sqlite3 Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX)
    target_link_libraries(soundpad PRIVATE m)
endif()

if(MSVC)
    target_compile_options(soundpad PRIVATE /W4 /utf-8 /permissive-)
    # Статический рантайм — exe запускается без VC++ Redistributable.
    set_property(TARGET soundpad sqlite3 PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(soundpad PRIVATE -Wall -Wextra)
    if(MINGW)
        target_link_options(soundpad PRIVATE -static)
    endif()
endif()


# Предупреждения сторонних библиотек не интересны.
if(MSVC)
    set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    set_source_files_properties(src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
    target_compile_options(sqlite3 PRIVATE -w)
endif()
