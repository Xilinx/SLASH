# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

# DFX compatibility check, shared by the slash and service-layer RM builds.
#
# Nothing in the flow used to check that a reconfigurable module is loadable
# into the shell it was built against, which is how the NoC aperture mismatch
# in issue 228 shipped in every example unnoticed.
#
# What it fails on is deliberately narrow. A NOC-Validation violation means the
# reconfigurable NoC boundary disagrees with the static shell, which makes the
# image non-loadable and is the defect this check exists to catch -- that one
# is fatal, and the partial PDI is deleted so it cannot reach a .vbin (slashkit
# treats a missing PDI as a build failure).
#
# Every other pr_verify failure is reported loudly and left to the operator.
# The coverage behind this check is three designs on two shells and a single
# tool version, which is not enough to assert that some other pr_verify error
# makes an image unusable -- and failing a build that works on hardware is
# worse than a warning that gets read. Tighten this once there is evidence.
#
# Note HDPRVerify-40 ("different site programming for static") is a warning,
# not an error: it is an artefact of comparing an abstract shell against a
# fully routed design and appears on passing builds, so it never reaches here.

proc slash_pr_verify {static_dcp routed_dcp report partial_pdi label} {
    puts "PR_VERIFY: $label: $static_dcp vs $routed_dcp"

    if {![catch {pr_verify -full_check -file $report $static_dcp $routed_dcp} msg]} {
        puts "PR_VERIFY: $label: compatible -- report at $report"
        return
    }

    # Did it fail for the reason this check understands?
    set noc_violation 0
    if {[file exists $report]} {
        set fh [open $report r]
        set txt [read $fh]
        close $fh
        if {[string first "NOC-Validation" $txt] >= 0} {
            set noc_violation 1
        }
    }

    if {$noc_violation} {
        file delete -force $partial_pdi
        error "DFX compatibility check failed for $label: the reconfigurable NoC\
 boundary does not match the static shell, so this image is not loadable. The\
 partial PDI has been deleted. Report: $report\n$msg"
    }

    send_msg_id "SLASH 1-1" "CRITICAL WARNING" \
        "pr_verify reported $label incompatible with its static shell for a\
 reason other than a NoC boundary mismatch. The partial PDI has been KEPT and\
 the build continues, because this check is not yet trusted to distinguish a\
 genuinely unloadable image from a benign difference. Inspect $report before\
 relying on this image.\n$msg"
}
