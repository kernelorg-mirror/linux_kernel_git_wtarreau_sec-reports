#!/bin/bash

MSG="$1"
export LC_ALL=C
KDIR="${KDIR:-/usr/src/linux}"
#CID=$(dd if=/dev/urandom bs=16 count=1 status=none | od -tx1 -An | tr -dc '[0-9a-f]')
DB_PATH=$(llm logs path)

llm -m local -s "Analyze this email received on the linux kernel security list (attachments were dropped before passing it to you). Once done, you will emit a extra header 'x-file:' followed by the file names affected by the bug report, or, if it was not possible to figure affected file names, 'x-func:' with the affected function(s). Then I will process your responses and will come back with new instructions." < "$MSG" > "$MSG".loc

CID=$(sqlite3 "$DB_PATH" "SELECT id FROM conversations ORDER BY rowid DESC LIMIT 1;")

llm -m local --cid "$CID" -c "Emit a line starting with 'x-subsys:' followed by the name of thesubsystem affected by this report." > "$MSG".subsys

llm -m local --cid "$CID" -c "Emit a line starting with 'x-summary:' followed by a quick summary of the claims of the report for maintainers. The goal is to have just one line of a few sentences to help maintainers figure if it's for them or for someone else." > "$MSG".summary

#if ! grep -q '^x-file:' "$MSG".loc; then
#  echo "no location"
#  exit 1
#fi

maint=( )
if grep -q '^x-file:' "$MSG".loc; then
    files=( $(grep '^x-file:' "$MSG".loc | cut -f2- -d: | tr ',' ' ') )
    for f in "${files[@]}"; do
        f="${f##[.ab]/}"
        maint[${#maint[@]}]=$(cd "$KDIR"; ./scripts/get_maintainer.pl "$f")
    done
fi

if [ ${#maint[@]} -gt 0 ]; then
	llm -m local --cid "$CID" -c >"$MSG".maint <<EOF
I checked the referenced files with get_maintainers and got the following enclosed between input tags for each file:
<input>
$(for ((i=0; i<${#maint[@]}; i++)); do echo "*file ${files[i]}:*"; echo "${maint[i]}";echo; done)
</input>
I'd like to get the maintainers' name+email on a line starting with 'x-cc:' and all delimited by a comma, so that I could easily turn that into a 'Cc:' header once I validate it. For each file, include all maintainers listed under the most specific and highest-priority role that appears first in the input. If multiple maintainers share the exact same most-specific role, include all of them. If a same maintainer appears for multiple files (which is common), print it only ones. Never dump reviewer nor list addresses!
EOF
else
	touch "$MSG".maint
fi

./append-lines -H "In-reply-to: $(sed -n '/^Message-Id:/Is,^[^:]*:[ ]*,,p' "$MSG")" -H "X-ai-processed: true" -B "--- automatically added below ---" -B "Subsystem: $(sed -n '/^x-subsys:/s,^[^:]*:[ ]*,,p' "$MSG.subsys")" -B "Cc: $(sed -n '/^x-cc:/s,^[^:]*:[ ]*,,p' "$MSG.maint")" -B "Summary: $(sed -n '/^x-summary:/s,^[^:]*:[ ]*,,p' "$MSG".summary)" -B "" -B "Thanks for your report. We've forwarded your original message to the maintainers and added them in Cc." -B "" -B "Since you've done all the analysis, do you have a patch to propose to fix this issue ? This would save some maintainers' time and you'd get full credit for finding and fixing this bug. For guidance on how to write patches, please see Documentation/process/submitting-patches.rst." -B "--- automatically added above ---" -B "" < "${MSG}" > "${MSG}.edited"

# purge conversations related to this $CID
(sqlite3 "$DB_PATH" "DELETE FROM responses WHERE conversation_id = '$CID';"
 sqlite3 "$DB_PATH" "DELETE FROM conversations WHERE id = '$CID';"
) >/dev/null 2>&1

exit 0
