`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================

.. biblioklept

Experimental Android kbase PanVK fork
--------------------------------------

This fork adds an experimental PanVK backend for proprietary Android ``kbase``
drivers, including the Tensor G1 Mali-G78 Job Manager path. It is intended for
arm64 PRoot, chroot, and similar Linux environments which can access
``/dev/mali0``. It is not a universal Mali driver and is not Vulkan-conformant.

Build and use
~~~~~~~~~~~~~

Start by checking that the Android application domain exposes the GPU node::

  $ uname -m
  $ ls -lZ /dev/mali0 /dev/dma_heap/system

Clone the tested development branch::

  $ git clone https://github.com/SaicharanKandukuri/tensor-g1-proot-gpu.git
  $ cd tensor-g1-proot-gpu
  $ git switch research/pixel6a-panvk-v9-jm

The `PRoot build and run guide <docs/panvk-kbase.md>`_ contains the exact
Ubuntu 24.04 dependencies, Meson configuration, package installation, required
mounts, Termux:X11 setup, Zink configuration, troubleshooting, and all supported
runtime switches.

After installing the ICD, the minimum Vulkan environment is::

  $ export DISPLAY=unix/:0
  $ export VK_DRIVER_FILES=/opt/panvk-kbase/share/vulkan/icd.d/panfrost_icd.aarch64.json
  $ export PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1
  $ export WSI_X11_TERMUX=1
  $ vulkaninfo --summary
  $ vkcube

If the X server does not support the private raw dma-buf transport, use
``PANVK_KBASE_DRI3=0`` for the slower MIT-SHM fallback. PRoot cannot bypass
Android permissions or SELinux; seeing a device node does not guarantee that
its ioctls are permitted.

Engineering status and validated hardware are tracked in the
`Tensor G1 driver roadmap <docs/tensor-g1-gpu-roadmap.md>`_ and
`research ledger <docs/tensor-g1-gpu-research.md>`_.


Source
------

Upstream Mesa lives at https://gitlab.freedesktop.org/mesa/mesa. This
experimental fork lives at
https://github.com/SaicharanKandukuri/tensor-g1-proot-gpu and its kbase changes
are not supported by the upstream Mesa project.


Build & install
---------------

You can find more information in our documentation (`docs/install.rst
<https://docs.mesa3d.org/install.html>`_), but the recommended way is to use
Meson (`docs/meson.rst <https://docs.mesa3d.org/meson.html>`_):

.. code-block:: sh

  $ meson setup build
  $ ninja -C build/
  $ sudo ninja -C build/ install

Support
-------

Many Mesa devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #dri-devel
<irc://irc.oftc.net/dri-devel>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.
To figure out who you're talking to, or which nick to ping for your
question, check out `Who's Who on IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

The next best option is to ask your question in an email to the
mailing lists: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_


Bug reports
-----------

If you think something isn't working properly, please file a bug report
(`docs/bugs.rst <https://docs.mesa3d.org/bugs.html>`_).


Contributing
------------

Contributions are welcome, and step-by-step instructions can be found in our
documentation (`docs/submittingpatches.rst
<https://docs.mesa3d.org/submittingpatches.html>`_).

Note that Mesa uses gitlab for patches submission, review and discussions.
