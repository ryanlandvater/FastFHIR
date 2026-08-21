#!/usr/bin/env bash
#
# Renders every AsciiDoc source under docs/ to HTML and PDF.
#
#   ./docs/build_docs.sh            # render all of docs/*.adoc into docs/output/
#   ./docs/build_docs.sh api.adoc   # render one document
#
# ASCIIDOCTOR'S DEFAULT EXIT CODE IS NOT A GATE. Measured on this tree: an
# include pointing at a file that does not exist prints an ERROR, writes the
# literal text "Unresolved directive" into the rendered output, and STILL EXITS
# 0 -- a document that publishes with a hole in it, on a green build.
#
# Three checks cover the include machinery here, and the redundancy is
# deliberate -- each catches a failure the others cannot see:
#   1. --failure-level=WARN, which turns the ERROR into a non-zero exit. This is
#      what fires today.
#   2. grepping the rendered HTML for "Unresolved directive". This is the
#      backstop: it is what catches the failure if anyone ever drops the flag in
#      (1) -- verified by rendering without it, where asciidoctor exits 0 and
#      only the grep sees the hole.
#   3. grepping for a printed "include::". A directive not at column 0 is not a
#      directive at all -- Asciidoctor prints it as text, exits 0, and (2) stays
#      silent because nothing was left unresolved: nothing was ever resolved.
#      This is the one that ships quietly; IFE lost six value tables to it.
# One flag is not a gate when removing it silently disables the gate.
#
# Same reasoning for orphaned generated tables: a renamed block leaves its old
# .adoc on disk, the narrative keeps including it, and the document ships a
# table nothing generates any more. Both traps are TASKS.md I1.10; both were
# paid for once already in the Iris File Extension and are not worth paying for
# twice.
#
# This Source Code Form is subject to the terms of the Mozilla Public License,
# v. 2.0 (MPL-2.0) -- see LICENSE or http://mozilla.org/MPL/2.0/.

set -euo pipefail

readonly DOCS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${DOCS_DIR}/.." && pwd)"
readonly OUT_DIR="${DOCS_DIR}/output"

die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
info() { printf '  %s\n' "$*"; }

# ── Toolchain ─────────────────────────────────────────────────────────────
# Fail with the install line rather than a bare "command not found": the
# dependency is Ruby-side and not obvious from a C++ repository.
for tool in asciidoctor asciidoctor-pdf; do
    command -v "${tool}" >/dev/null 2>&1 || die \
        "${tool} not found. Install with:  gem install asciidoctor asciidoctor-pdf
       (needs Ruby >= 3.2; see TASKS.md I1.12 for the toolchain decision)"
done

# ── Provenance ────────────────────────────────────────────────────────────
# Read the version the way the BUILD does, not the way the header does: CMake
# passes FASTFHIR_VERSION_* as compile definitions, so its defaults are what a
# shipped binary reports. include/FF_Version.hpp's #ifndef fallbacks only apply
# to builds that bypass CMake, and they do not currently agree (see the note at
# the end of this script).
cmake_default() {
    sed -n "s/^ *set(FASTFHIR_VERSION_$1 \"\([0-9]*\)\")/\1/p" \
        "${REPO_ROOT}/CMakeLists.txt" | head -1
}
VERSION_MAJOR="${FASTFHIR_VERSION_MAJOR:-$(cmake_default MAJOR)}"
VERSION_MINOR="${FASTFHIR_VERSION_MINOR:-$(cmake_default MINOR)}"
VERSION_BUILD="${FASTFHIR_VERSION_BUILD:-$(cmake_default BUILD)}"
[ -n "${VERSION_MAJOR}" ] && [ -n "${VERSION_MINOR}" ] && [ -n "${VERSION_BUILD}" ] \
    || die "could not read FASTFHIR_VERSION_* defaults from CMakeLists.txt"

readonly ENGINE_VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_BUILD}"
readonly TOOL_VERSION="$(asciidoctor --version | head -1)"
readonly BUILD_DATE="$(date -u '+%Y-%m-%d')"
# A document that cannot be traced back to the tree and toolchain that produced
# it is not reproducible in any useful sense (I1.10).
readonly GIT_REV="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo 'not-a-git-checkout')"

# ── Sources ───────────────────────────────────────────────────────────────
declare -a SOURCES=()
if [ "$#" -gt 0 ]; then
    for arg in "$@"; do
        src="${DOCS_DIR}/${arg#docs/}"
        [ -f "${src}" ] || die "no such document: ${arg}"
        SOURCES+=("${src}")
    done
else
    # Top level only. Generated include fragments live in subdirectories and
    # are rendered as part of the document that includes them, never alone.
    while IFS= read -r src; do SOURCES+=("${src}"); done \
        < <(find "${DOCS_DIR}" -maxdepth 1 -name '*.adoc' | sort)
fi
[ "${#SOURCES[@]}" -gt 0 ] || die "no .adoc sources found in ${DOCS_DIR}"

# ── Trap 2: orphaned generated fragments ──────────────────────────────────
# Every .adoc under a generated/ directory must be reachable by an include::
# from some top-level document. One that is not is either dead (a renamed block
# left it behind) or the narrative lost an include -- and neither shows up in
# the rendered output, which is what makes it worth checking mechanically.
check_orphans() {
    local generated_root="${DOCS_DIR}/generated"
    [ -d "${generated_root}" ] || { info "no docs/generated/ tree; orphan check not applicable"; return 0; }

    local orphans=0
    while IFS= read -r fragment; do
        local base; base="$(basename "${fragment}")"
        grep -rqF "include::" --include='*.adoc' "${DOCS_DIR}" \
            --exclude-dir=output -e "${base}" \
            || { warn "orphaned generated fragment, included by nothing: ${fragment#"${REPO_ROOT}"/}"; orphans=$((orphans + 1)); }
    done < <(find "${generated_root}" -name '*.adoc')

    [ "${orphans}" -eq 0 ] || die "${orphans} orphaned generated fragment(s); regenerate or delete them"
}

# ── Render ────────────────────────────────────────────────────────────────
mkdir -p "${OUT_DIR}"
check_orphans

mermaid_seen=0
for src in "${SOURCES[@]}"; do
    name="$(basename "${src}" .adoc)"
    html="${OUT_DIR}/${name}.html"
    pdf="${OUT_DIR}/${name}.pdf"
    info "rendering ${name}.adoc"

    common_attrs=(
        -a "revnumber=${ENGINE_VERSION}"
        -a "revdate=${BUILD_DATE}"
        -a "revremark=${GIT_REV} · ${TOOL_VERSION}"
        -a "ff-engine-version=${ENGINE_VERSION}"
        -a "ff-git-rev=${GIT_REV}"
        -a "ff-toolchain=${TOOL_VERSION}"
        --failure-level=WARN   # tightens the exit code; still not sufficient alone
    )

    asciidoctor "${common_attrs[@]}" -b html5 -o "${html}" "${src}"
    asciidoctor-pdf "${common_attrs[@]}" -o "${pdf}" "${src}"

    # ── Trap 1 backstop: the check a missing flag cannot disable ──────────
    if grep -qF "Unresolved directive" "${html}"; then
        printf '\n'
        grep -nF "Unresolved directive" "${html}" | head -5 >&2
        die "${name}.html contains unresolved include(s) — asciidoctor still exited 0"
    fi

    # ── Trap 1b: the quieter twin, and the one that actually ships ────────
    # `include::` is a directive ONLY at column 0. Anywhere else -- indented
    # under a list item, say -- Asciidoctor treats it as ordinary text and
    # prints it. There is no warning, the exit code is 0, --failure-level sees
    # nothing, and the check above stays silent for the worst possible reason:
    # nothing was left *unresolved*, because nothing was ever *resolved*.
    # Verified on this tree: an indented include renders as literal text and
    # every other gate here passes it. IFE shipped six value tables this way
    # before anyone noticed, which is the whole argument for checking it.
    if grep -qF "include::" "${html}"; then
        printf '\n'
        grep -o 'include::[^<)]*' "${html}" | sort -u | head -5 >&2
        die "${name}.html printed an include directive instead of processing it.
       include:: must begin at column 0. To attach a table to a list item, end
       the item, put a '+' on its own line, then the directive on the next."
    fi

    if grep -qF 'language-mermaid' "${html}"; then
        mermaid_seen=1
    fi

    info "  -> ${html#"${REPO_ROOT}"/}"
    info "  -> ${pdf#"${REPO_ROOT}"/}"
done

# Not fatal: whether Mermaid should render as a picture or as source is an open
# toolchain decision (TASKS.md I1.12), and failing the build over it would block
# doc generation on a question nobody has answered. But it must not pass
# silently either -- the style guide asks for diagrams, and a diagram that
# renders as its own source code is not one.
if [ "${mermaid_seen}" -eq 1 ]; then
    warn "Mermaid blocks rendered as highlighted SOURCE, not diagrams."
    warn "  asciidoctor-diagram is not loaded. To render them:"
    warn "    gem install asciidoctor-diagram && npm i -g @mermaid-js/mermaid-cli"
    warn "  then add '-r asciidoctor-diagram' here and use [mermaid] blocks."
    warn "  Decision is TASKS.md I1.12; leaving the output as-is until it lands."
fi

printf '\ndocs built: engine %s · %s · %s\n' "${ENGINE_VERSION}" "${GIT_REV}" "${TOOL_VERSION}"

# NOTE: CMakeLists.txt defaults to FASTFHIR_VERSION_MINOR=0 while
# include/FF_Version.hpp's #ifndef fallback is 1. They disagree, so a
# CMake-driven build reports 2026.0.0 and a hand-compiled TU reports 2026.1.0.
# This script follows CMake because that is what ships. Reported, not fixed --
# picking the winner is a release-metadata decision, not a docs one.
