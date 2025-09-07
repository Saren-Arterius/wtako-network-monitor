{
  "targets": [
    {
      "target_name": "metrics",
      "sources": [ "metrics.cpp" ],
      "include_dirs": [
        "<(module_root_dir)/node_modules/node-addon-api",
        "<(module_root_dir)/node_modules/napi-macros",
        "include"
      ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "10.7"
      },
      "msvs_settings": {
        "VCCLCompilerTool": { "ExceptionHandling": 1 }
      },
      "conditions": [
        ['OS=="mac"', {
          'cflags+': ['-fno-rtti'],
          'cflags_cc+': ['-fno-rtti'],
          'cflags_cc': ['-O3']
        }],
        ['OS=="win"', {
          'cflags+': ['-fno-rtti'],
          'cflags_cc+': ['-fno-rtti'],
          'cflags_cc': ['/O2'] # MSVC equivalent of -O3
        }],
        ['OS=="linux"', {
           "cflags_cc": [
             "-std=c++17",
             "-march=armv7-a",  # For Raspberry Pi 2 (ARMv7-A)
             "-mtune=cortex-a7", # For Raspberry Pi 2 (Cortex-A7)
             "-mfpu=neon-vfpv4", # Enable NEON FPU
             "-mfloat-abi=hard", # Hardware floating point
             "-O3"               # Highest optimization
           ]
        }]
      ],
      "defines": [ "NAPI_CPP_EXCEPTIONS" ]
    }
  ]
}