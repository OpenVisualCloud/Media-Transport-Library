=====================
MTL Test Suite
=====================

Welcome to the Media Transport Library (MTL) Test Suite documentation.

This documentation covers both Python-based validation tests (pytest) and C++ integration tests (gtest).

.. toctree::
   :maxdepth: 2
   :caption: Contents:

   pytest/index
   gtest/index

Overview
========

The MTL test suite validates functionality, performance, and reliability across various
network configurations and media formats.

Test Categories
===============

Python Tests (pytest)
---------------------

End-to-end validation using RxTxApp:

* **Kernel Socket Tests**: Using kernel networking stack
* **PMD+Kernel Hybrid**: DPDK PMD (TX) and kernel socket (RX)
* **Media Format Tests**: Video (ST2110-20), audio (ST2110-30), ancillary (ST2110-40)

C++ Tests (gtest)
-----------------

Low-level unit and integration tests:

* Component testing
* API validation  
* Performance measurements

Getting Started
===============

Prerequisites
-------------

Python Tests:
  * Python 3.8+, pytest with mfd plugins
  * MTL build with RxTxApp
  * Network interfaces (DPDK or kernel)

C++ Tests:
  * Google Test framework
  * MTL library built

Running Tests
-------------

Python::

    cd tests/validation
    python -m pytest tests/single/

C++::

    cd build
    ctest

    ./KahawaiUnitTest

Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
