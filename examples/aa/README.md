# A/A input

Two Zebrac captures of the same command (`run1.json`, `run2.json`) and an `input-manifest.tsv` that pairs them. It is input for a repeatability check, which `isocost` 1.0 does not perform: `isocost` itself rejects the pair as a duplicate identity.

The manifest columns are `source_json`, `result_command`, `comparison`, `data_id`, and `run_id` (`run1` or `run2`), plus optional identity columns. Run 2 takes 2% more time and 3% more memory than run 1.
