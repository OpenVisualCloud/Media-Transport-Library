==================
C++ Tests (gtest)
==================

Google Test framework for MTL core unit and integration tests.

.. note::
   Requires Doxygen XML output. Run Doxygen before building Sphinx docs.

Test Binaries
=============

* **KahawaiUnitTest**: Core functionality
* **KahawaiTestSuite**: Integration tests

Building and Running
====================

Build::

    meson setup build && cd build && meson compile

Run all tests::

    cd build && ctest

Run specific test::

    cd build
    ./KahawaiUnitTest --gtest_filter=TestPattern

List tests::

    ./KahawaiUnitTest --gtest_list_tests

Test Coverage
=============

* **API**: Session creation, parameter validation, error handling
* **Data Path**: Frame TX/RX, timestamps, pacing
* **Format**: Video conversions, color spaces, pixel formats

.. note::
   For detailed C++ API docs, configure Doxygen XML output in conf.py.
