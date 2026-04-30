#!/usr/bin/env bash

set -u
set -o pipefail

base_dir="$(pwd)"
declare -a result_lines=()
declare -a pass_dirs=()

# If empty => run all first-level subdirectories.
# If non-empty => run only these directory names.
# Example:
#   RUN_DIRS=("a" "b" "c")
RUN_DIRS=()

run_count=0

declare -a target_dirs=()
if [ "${#RUN_DIRS[@]}" -eq 0 ]; then
  for dir in "${base_dir}"/*/; do
    [ -d "${dir}" ] || continue
    target_dirs+=("$(basename "${dir%/}")")
  done
else
  target_dirs=("${RUN_DIRS[@]}")
fi

for subdir in "${target_dirs[@]}"; do
  dir="${base_dir}/${subdir}"
  if [ ! -d "${dir}" ]; then
    result_lines+=("${subdir}: FAIL (directory not found)")
    continue
  fi

  run_count=$((run_count + 1))

  echo "=== [${subdir}] running: make uvs ==="
  output_file="$(mktemp)"

  (
    cd "${dir}" || exit 1
    make uvs
  ) >"${output_file}" 2>&1
  make_rc=$?

  if grep -q "TEST PASS" "${output_file}"; then
    status="PASS"
    pass_dirs+=("${subdir}")
  else
    status="FAIL"
  fi

  result_lines+=("${subdir}: ${status} (make_rc=${make_rc})")

  echo "--- output (${subdir}) ---"
  cat "${output_file}"
  echo "--- end output (${subdir}) ---"
  echo

  rm -f "${output_file}"
done

echo "================ Summary ================"
if [ "${#target_dirs[@]}" -eq 0 ]; then
  echo "No subdirectories found under: ${base_dir}"
  exit 0
fi

for line in "${result_lines[@]}"; do
  echo "${line}"
done

echo "----------------------------------------"
echo "PASS count: ${#pass_dirs[@]}/${run_count}"
if [ "${#pass_dirs[@]}" -gt 0 ]; then
  echo "PASS dirs: ${pass_dirs[*]}"
fi
