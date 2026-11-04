# Provenance: agentic-actions-auditor

The files in this directory, apart from this one, are copied unmodified
from Trail of Bits' skills repository:

- Repository: https://github.com/trailofbits/skills
- Commit: `711af095215ad4ecde5b90cd1d0124b3f1c4ca9e` (plugin version 1.3.2)
- Source paths:
  - `plugins/agentic-actions-auditor/skills/agentic-actions-auditor/SKILL.md`
  - `plugins/agentic-actions-auditor/skills/agentic-actions-auditor/references/*.md`
  - `LICENSE` (repository root)
- Authors: Emilio López and Will Vandevanter, Trail of Bits.
- Licence: Creative Commons Attribution-ShareAlike 4.0 International (`LICENSE`).
  These files stay under that licence; the rest of ODBCPP is unaffected.
- Copied on 2026-11-04 for #193. Every file was read in full first. Each
  copy's git blob hash equals the blob in the upstream tree at that commit.

Left out:
- `assets/trail-of-bits-mark.svg`: a logo. SVG can carry scripts, and the skill
  doesn't need it.
- `agents/openai.yaml`: Codex display metadata that only points at the logo.
- The plugin's `README.md` and `.claude-plugin/plugin.json`: marketplace
  packaging, not used here.

What it does: static review of `.github/workflows/*.yml` for paths by which
attacker-controlled input reaches an AI agent in CI. Its only shell use is
read-only `gh api` fetches. It reports findings and changes no files.

## Updating

Never edit these files in place. To update:
1. Pick a new upstream commit.
2. Read the upstream diff in full.
3. Copy the files again.
4. Update the commit and the hashes below.
5. Open a PR. It needs the owner's approval, because `.agents/` is in CODEOWNERS.

## SHA-256 of each copied file

```
7abe19ec9bb73b36141b999b861d24ad855e808bafe0f81e84cce28556f6c297  LICENSE
b2bdb88d559bc37e6c2c8314e28e7a2eb47e7cdd9da51e77da347d3517326dc0  SKILL.md
bbfaf6abe69b57e6289948340aa649482378b53f752a25cba92e7d4a740c4324  references/action-profiles.md
b7d0d31fa24f1b987953ef0c11dc22ad48e81675b34de65c65ff79a74a3e6084  references/cross-file-resolution.md
722ee7ea1dc06af8fa8cbb6cacb62f3c7c70139c8ed402752dd12c851a195d0e  references/foundations.md
7903992f54409760aae64ddb45bb3c26032223443f746ac6e3d8fd281bda877d  references/vector-a-env-var-intermediary.md
be6e5baff68d6770665ce6c7b7995c92dfdec3866b96ea21330c94b340499631  references/vector-b-direct-expression-injection.md
f11d2333f2b3800215311047c0b0ad35a1020c79dbb77614ff66a8c1fef16252  references/vector-c-cli-data-fetch.md
fc029037bb219aed5e130708eb4911ad235e47f1394e2155b5e40175ddb2d187  references/vector-d-pr-target-checkout.md
2c93623d96f49c4ab70bc2dd92e111b2cfca68ff70aaf579efd86812b0df15a9  references/vector-e-error-log-injection.md
d6ede1d6a7769e1b75faed3dfeba7d212e0115fb9ddb09d39145bf65d062ba7f  references/vector-f-subshell-expansion.md
b9a47a41dd1470cc469cc030cd6cd6059b35741c78ba9f494c1d18b2fd65b686  references/vector-g-eval-of-ai-output.md
892cb478d91c2e155b4084c5cf707bea5045bb4e2a11c7a5a1131b6212690b86  references/vector-h-dangerous-sandbox-configs.md
68a4cd774ad829e535a8ec15ee492caefbae8e46f115f345b7f2bf2fecf88f86  references/vector-i-wildcard-allowlists.md
```
