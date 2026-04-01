#!/bin/bash

MSG="$1"
export LC_ALL=C
KDIR="${KDIR:-/usr/src/linux}"
AGENT="${AGENT:-local}"
DB_PATH=$(llm logs path)

llm -m "$AGENT" -s "Analyze this email received on the linux kernel security list (attachments were dropped before passing it to you). Once done, you will emit a header 'x-file:' followed by the file names affected by the bug report, or, if it was not possible to figure affected file names, 'x-func:' with the affected function(s). Then I will process your responses and will come back with new instructions." < "$MSG" > "$MSG".loc

CID=$(sqlite3 "$DB_PATH" "SELECT id FROM conversations ORDER BY rowid DESC LIMIT 1;")

llm -m "$AGENT" --cid "$CID" -c "Emit a line starting with 'x-version:' followed by the version(s) or the range of versions of kernels affected by this report, or 'unknown' if not found." > "$MSG".version

llm -m "$AGENT" --cid "$CID" -c "Emit a line starting with 'x-subsys:' followed by the name of the subsystem affected by this report." > "$MSG".subsys

llm -m "$AGENT" --cid "$CID" -c "Emit a line starting with 'x-summary:' followed by a quick summary of the claims of the report for maintainers. The goal is to have just one line of a few sentences to help maintainers figure if it's for them or for someone else." > "$MSG".summary

files=( )
maint=( )
cc=( )
maint_all=""
if grep -q '^x-file:' "$MSG".loc; then
    files=( $(grep '^x-file:' "$MSG".loc | cut -f2- -d: | tr ',' ' ' | tr ' ' '\n' | fgrep -vw "n/a" | sort -u) )
    for f in "${files[@]}"; do
        f="${f##[.ab]/}"
        o="$MSG.maint.${#maint[@]}"
        # first get only maintainers of that specific sub-system. A few
        # sometimes leak names (e.g. akpm) so use sed to extract the address.
        a=$(cd "$KDIR"; ./scripts/get_maintainer.pl --no-tree --no-l --no-r --no-n --m  --no-git-fallback --pattern-depth 1 --no-substatus --no-rolestats "$f" 2>/dev/null | sed 's/^[^<]*<\([^>]*\)>/\1/')
        # let's also build a multi-level maintainers list. If none is found
        # for a file, let's involve git as well (not frequent).
        m=$(cd "$KDIR"; ./scripts/get_maintainer.pl --no-tree --no-l --no-r --m --no-git-fallback --no-substatus "$f" 2>/dev/null)
        if [ -z "$m" ]; then
            m=$(cd "$KDIR"; ./scripts/get_maintainer.pl --no-tree --no-l --no-r --m --no-substatus "$f" 2>/dev/null)
        fi
        (echo "# file: $f"; echo "$m") > "$o"
        maint[${#maint[@]}]="$m"
        maint_all="${maint_all}${m}"
        cc[${#cc[@]}]="$a"
    done
fi

for ((i=0; i<${#cc[@]}; i++)); do echo "${cc[i]}"; done | grep . | sort -u > "$MSG.cc"
cc_all=$(echo $(cat "$MSG.cc") | sed -e 's: :, :g')

for ((i=0; i<${#maint[@]}; i++)); do echo "${maint[i]}"; done | grep . | sort -u > "$MSG.maint"
maint_all="$(cat "$MSG.maint")"

./append-lines -H "In-reply-to: $(sed -n '/^Message-Id:/Is,^[^:]*:[ ]*,,p' "$MSG")" -H "Cc: $cc_all" -H "X-ai-processed: true" -B "Hello," -B "" -B "Thanks for your report. The maintainers have been added in Cc. A few comments below." -B "Thanks for your report. We've forwarded your original message to the maintainers and added them in Cc. A few comments below." -B "" -B "Version: $(sed -n '/^x-version:/s,^[^:]*:[ ]*,,p' "$MSG.version")" -B "Subsystem: $(sed -n '/^x-subsys:/s,^[^:]*:[ ]*,,p' "$MSG.subsys")" -B "Files: ${files[*]}" -B "-- full maintainers list --" -B "$(cat "$MSG.maint")" -B "--" -B "Summary: $(sed -n '/^x-summary:/s,^[^:]*:[ ]*,,p' "$MSG".summary)" -B "" -B "Could you please turn your fix into a patch that can directly be applied ? This would save some maintainers' time and if accepted, you'd get full credit for finding and fixing this bug. For guidance on how to write patches, please see Documentation/process/submitting-patches.rst." -B "Since you've done all the analysis, do you have a patch to propose to fix this issue ? This would save some maintainers' time and if accepted, you'd get full credit for finding and fixing this bug. For guidance on how to write patches, please see Documentation/process/submitting-patches.rst." -B "" -B "--- original message below this line ---" -B "" < "${MSG}" > "${MSG}.edited"

# purge conversations related to this $CID
(sqlite3 "$DB_PATH" "DELETE FROM responses WHERE conversation_id = '$CID';"
 sqlite3 "$DB_PATH" "DELETE FROM conversations WHERE id = '$CID';"
) >/dev/null 2>&1

exit 0
