# Sample code for how to develop application in MSVC

## 1. Introduction

The is sample code for how to develop MSVC application quickly based on Media Transport Library. The guide is verified with Visual Studio 2022 (v143).

## 2. Steps

### 2.1 Prepare latest .lib file

* Build IMTL library in MSYS2, see [WIN build guide](../../../doc/build_WIN.md), also need to add MSYS2 binary PATH to system environment variables.

* After build, in the build folder, you will get `libmtl.def` file.

* Open Developer Command Prompt for VS here, run below command:

    ```powershell
    lib /machine:x64 /def:libmtl.def
    ```

* Copy `libmtl.lib` to project folder.

### 2.2 Set project properties

* Double-click `imtl_sample.sln` to launch VS or create a console app in VS with the `imtl_sample.cpp`.

* In Solution Explorer, right click on project, open Properties window, choose the configuration needed, such as `Release - x64`.

* Go to VC++ Directories, add MSYS2 Environment include folder(eg. for UCRT64, `C:\msys64\ucrt64\include`) to `External Include Directories`.

* Go to Linker - Input, add `libmtl.lib` to `Additional Dependencies`

* Click `OK` or `Apply` to save properties.

### 2.3 Build and run

* Inside IDE: Click Run button (Local Windows Debugger/ Start Without Debugging) to build and run the application.

* Run binary: After build, go to output folder(eg. x64\Release), double-click `imtl_sample.exe` to run.
