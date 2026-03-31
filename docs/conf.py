# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
# 
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
# 
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# http://www.sphinx-doc.org/en/master/config

# -- Path setup --------------------------------------------------------------

# If extensions (or modules to document with autodoc) are in another directory,
# add these directories to sys.path here. If the directory is relative to the
# documentation root, use os.path.abspath to make it absolute, like shown here.
#
# import os
# import sys
# sys.path.insert(0, os.path.abspath('.'))

import os
import subprocess

# -- Project information -----------------------------------------------------

project = 'SLASH'
copyright = '2025 Advanced Micro Devices, Inc'
author = 'Advanced Micro Devices Inc'

breathe_projects = {
    'VRT':       '../vrt/doc/docs/xml',
    'libslash':  '../driver/libslash/doc/docs/xml',
    'libvrtd':   '../vrt/vrtd/libvrtd/doc/docs/xml',
    'libvrtdpp': '../vrt/vrtd/libvrtdpp/doc/docs/xml',
}

# Check if we're running on Read the Docs' servers
read_the_docs_build = os.environ.get('READTHEDOCS', None) == 'True'

# -- General configuration ---------------------------------------------------

# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.
#...

extensions = [ "breathe", 'sphinxcontrib.tikz', 'sphinx.ext.todo', 'sphinx_design']

#...

# Add any paths that contain templates here, relative to this directory.
templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path.
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']


# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
#
html_theme = 'sphinx_rtd_theme'

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = ['_static']

# Breathe Configuration
breathe_default_project = "SLASH"
breathe_domain_by_extension = {"h": "c"}

# -- Doxygen builds (generate XML for Breathe) --------------------------------

subprocess.call('doxygen ./Doxyfile', shell=True, cwd='../vrt/doc')
subprocess.call('doxygen ./Doxyfile', shell=True, cwd='../driver/libslash/doc')
subprocess.call('doxygen ./Doxyfile', shell=True, cwd='../vrt/vrtd/libvrtd/doc')
subprocess.call('doxygen ./Doxyfile', shell=True, cwd='../vrt/vrtd/libvrtdpp/doc')

# -- Tikz configuration -------------------------------------------------------
# As indicated in https://github.com/sphinx-contrib/tikz
tikz_proc_suite = 'GhostScript'

# -- Todo extension ------------------------------------------------------------
todo_include_todos = True
