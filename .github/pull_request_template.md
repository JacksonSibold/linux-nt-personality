## Summary

<!-- What changed and why, in a few sentences. Link the issue: Closes #NNN -->

## Source

<!-- The repository document or finding this change comes from (relative path or a permalink to main). -->

## Checks run

<!-- Paste the exact commands and the result line of each. Mark what was not run. -->

- [ ] `python -m unittest discover tests/unit`
- [ ] `python tools/check_doc_links.py` (if docs changed)
- [ ] `python tools/check_repo.py`
- [ ] Host C tests (name them): 
- [ ] Guest or VM run (name the boot and the log): not run / run

## Criteria and results

<!-- If a criterion status is affected, say whether dodrun recorded it. A PASS needs a dodrun record, not a direct check. Otherwise write "none". -->

## Not verified

<!-- Everything you read but did not run, and anything that needs a guest or an owner decision. -->

## Checklist

- [ ] No secrets, tokens, machine-specific paths or personal data in the diff.
- [ ] Generated files were regenerated with their script, not edited by hand.
- [ ] Line endings are consistent within each file.
