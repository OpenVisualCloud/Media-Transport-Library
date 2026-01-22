=======================
Kernel Socket Tests
=======================

Tests for kernel-based networking backends (native drivers and AF_XDP).

PMD + Kernel Tests
==================

Hybrid configurations combining DPDK PMD with kernel socket. Require dual-interface topology on same physical NIC.

Video Tests
-----------

.. automodule:: tests.single.kernel_socket.pmd_kernel.video.test_pmd_kernel_video
   :members:
   :undoc-members:
   :show-inheritance:

Mixed Media Tests
-----------------

.. automodule:: tests.single.kernel_socket.pmd_kernel.mixed.test_pmd_kernel_mixed
   :members:
   :undoc-members:
   :show-inheritance:

Kernel Loopback Tests
=====================

Tests using kernel socket over loopback interface (kernel:lo) for local validation without physical NICs.

Mixed Media Loopback
--------------------

.. automodule:: tests.single.kernel_socket.kernel_lo.test_kernel_lo
   :members:
   :undoc-members:
   :show-inheritance:

ST2110-20 Pipeline Loopback
----------------------------

.. automodule:: tests.single.kernel_socket.kernel_lo_st20p.test_kernel_lo_st20p
   :members:
   :undoc-members:
   :show-inheritance:

ST2110-22 JPEG XS Loopback
---------------------------

.. automodule:: tests.single.kernel_socket.kernel_lo_st22p.test_kernel_lo_st22p
   :members:
   :undoc-members:
   :show-inheritance:
