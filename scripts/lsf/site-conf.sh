#!/bin/bash
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
#
# Shared configuration loader for the LSF example launcher. Sourced by the
# other scripts in this directory; it does nothing on its own.
#
# Everything that identifies a particular cluster -- queue names, OS selectors,
# the path to the tool installation -- lives in a site.conf that is not part of
# this repository. See site.conf.example for the full list of keys.

# Knobs that can be given a per-task value, i.e. SLASH_LSF_MEM_MB_bootgen as
# well as SLASH_LSF_MEM_MB.
_SLASH_TUNABLES=(QUEUE MEM_MB CORES WALLTIME OSTYPE SETTINGS)

slash_die() {
    echo "ERROR: $*" >&2
    exit 2
}

# Set up SLASH_TASK and load the site configuration. Pass the directory holding
# these scripts.
slash_site_init() {
    local dir="$1" conf key var

    SLASH_TASK="${SLASH_BUILD_TASK:-default}"
    if [[ ! "$SLASH_TASK" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
        # The task name becomes part of a variable name below, so a stray
        # value would turn into a confusing bash error instead of a clear one.
        slash_die "SLASH_BUILD_TASK is not a valid identifier: '$SLASH_TASK'"
    fi

    # Anything already exported is a deliberate one-off override, e.g.
    # "SLASH_LSF_MEM_MB=120000 slashkit link ...". Remember which keys those
    # are, so that a per-task value from the configuration does not quietly
    # beat them.
    _SLASH_PRESET=""
    for key in "${_SLASH_TUNABLES[@]}"; do
        var="SLASH_LSF_$key"
        [[ -n "${!var:-}" ]] && _SLASH_PRESET="$_SLASH_PRESET $key"
    done

    SLASH_SITE_CONF_HINT="$dir/site.conf (start from $dir/site.conf.example)"
    if [[ -n "${SLASH_LSF_SITE_CONF:-}" ]]; then
        # Explicitly named but unusable: never fall through to the defaults,
        # since those are what the caller was trying to replace.
        conf="$SLASH_LSF_SITE_CONF"
        [[ -r "$conf" ]] || slash_die \
            "SLASH_LSF_SITE_CONF names a file that cannot be read: $conf"
    else
        conf="$dir/site.conf"
        [[ -r "$conf" ]] || return 0
    fi

    SLASH_SITE_CONF="$conf"
    # shellcheck disable=SC1090
    source "$conf"
}

# Value of one tunable for the current task: SLASH_LSF_<KEY>_<task> if the site
# set one, else the site-wide SLASH_LSF_<KEY>. Prints nothing if neither is set.
slash_task_value() {
    local key="$1" generic="SLASH_LSF_$1" specific="SLASH_LSF_$1_$SLASH_TASK"
    if [[ " $_SLASH_PRESET " == *" $key "* ]]; then
        printf '%s' "${!generic}"
    else
        printf '%s' "${!specific:-${!generic:-}}"
    fi
}

# As above, but fail with an actionable message rather than guessing. There are
# deliberately no built-in defaults for the sizing knobs: a reservation that is
# wrong by default either wastes the cluster's capacity or gets a twelve-hour
# implementation run OOM-killed in hour three.
slash_require() {
    local key="$1" value
    value="$(slash_task_value "$key")"
    if [[ -z "$value" ]]; then
        slash_die "$(cat <<EOF
SLASH_LSF_$key is not set and has no default, because the right value depends
on your cluster. Set it in ${SLASH_SITE_CONF:-$SLASH_SITE_CONF_HINT}, either
site-wide as SLASH_LSF_$key or for this step alone as
SLASH_LSF_${key}_${SLASH_TASK}.
EOF
)"
    fi
    printf '%s' "$value"
}

# Drop entries of a colon-separated path list that are not readable here.
slash_prune_pathlist() {
    local kept=() entry entries
    IFS=':' read -ra entries <<< "$1"
    for entry in "${entries[@]}"; do
        [[ -n "$entry" && -r "$entry" ]] && kept+=("$entry")
    done
    (IFS=':'; printf '%s' "${kept[*]}")
}
