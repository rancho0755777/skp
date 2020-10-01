1. 编译方法

```shell
> git clone https://github.com/crazyherozk/skp.git
> cd skp 
> mkdir build
> cd build
> cmake .. -DCMAKE_BUILD_TYPE=Debug/Release -DBUILD_SHARED_LIBS=OFF/ON \
    -DINSTALL_TEMP=OFF/ON -DBUILD_TESTS=OFF/ON -DUMALLOC_MANGLE=OFF/ON \
    -DENABLE_SSL=ON/OFF
```

如果使用 macOS 平台 并需要生成 Xcode 项目 那么 请追加 `-GXcode` 选项，如果使用 Xcode 12 那么 `-DCMAKE_OSX_ARCHITECTURES=x86_64` 是必须的追加的选项

   * `INSTALL_TEMP` 选项表明是否安装在 `/tmp/install` 目录下，默认关闭，库将被安装在 `/usr/local/` 下。
   * `BUILD_SHARED_LIBS` 选项是否编译为动态库，默认是开启的。
   * `BUILD_TESTS` 是否编译测试程序，并且可以使用 `cmake test` 进行库的基础测试，默认关闭的。
   * `UMALLOC_MANGLE` 是否启用 skp 内部实现的内存分配器，这个分配器完全参考了内核的伙伴系统和SLUB子系统的实现，默认是开启的。
   * `ENABLE_SSL` 传送对象是否启用对 `openssl` 的支持，默认关闭的。 

2. 库的使用方法

你可以参考下面的 cmake 片段来使用本库

```
cmake_minimum_required(VERSION 3.5)
project(skp-test)

if(INSTALL_TEMP)
    list(APPEND CMAKE_PREFIX_PATH "/tmp/install/lib/cmake")
endif()

find_package(skp REQUIRED)
add_executable(skp-test main.c)
target_include_directories(skp-test PRIVATE ${SKP_INCLUDE_DIR})
target_link_libraries(skp-test PRIVATE skp::skp)
```