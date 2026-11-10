# SPDX-License-Identifier: MIT
#
# ksre-ps5: stand-in for Ren'Py's renpy.gl2.assimp on builds without the assimp library.
#
# Ren'Py 8.5.3 imports renpy.gl2.assimp unconditionally (renpy/__init__.py) and calls
# free_memory(), finish_predict() and preload() every frame/prediction cycle; the store
# exposes GLTFModel. The real module is Cython code linked against assimp (C++), which
# the PS5 port does not build. This module keeps that interface: the hooks do nothing,
# and creating a GLTFModel raises an error that names the reason. Installed by
# ports/renpy/build.sh as renpy/gl2/assimp.py in the console's Ren'Py tree.
# Written under Ren'Py's licence (MIT) so it could be offered upstream.

import threading

loader = None
"No model loader on this build."

loader_lock = threading.Lock()


class GLTFModel(object):
    """
    Placeholder for renpy.gl2.assimp.GLTFModel: 3D models need the assimp library,
    which this build of Ren'Py does not include.
    """

    def __init__(self, filename, *args, **kwargs):
        raise Exception("GLTFModel(%r): this Ren'Py build has no assimp support." % (filename,))


def free_memory():
    return


def finish_predict():
    return


def preload():
    return
