# 15. DMF CRM manifest generation

Before proceeding:

* PerfSpect baseline collected — see [04-perfspect-baseline.md](04-perfspect-baseline.md)
* Profiling run completed — see [08-profiling-manifest.md](08-profiling-manifest.md)

---

## What the generator creates

A `MxlMediaFunctionParameters` YAML manifest conforming to the bundled
[AMWA-TV/jt-dmf-crm schema](https://github.com/AMWA-TV/jt-dmf-crm/blob/main/manifest/schema/resource_manifest_schema.yaml).

The host capability fields are written to:

```yaml
spec:
  requirements:
    host_capabilities:
      cpu:
        features: []
      memory: []
```

---

## Command

```text
usage: create-dmf-manifest [-h] --perfspect DIR --profile DIR --service SERVICE
                           [--name NAME] [--output OUTPUT] [--namespace NAMESPACE]
                           [--cpu-margin FACTOR] [--memory-margin FACTOR]
                           [--schema FILE] [--no-validate] [--dry-run]
```

| Option | Meaning |
|---|---|
| `--perfspect DIR` | **Required.** PerfSpect baseline directory or JSON file |
| `--profile DIR` | **Required.** Profiling run directory containing `metrics.csv` and `config.json` |
| `--service SERVICE` | **Required.** Service to select from `metrics.csv`; written to `spec.role` |
| `--name NAME` | Override `metadata.name` (default: `<service>-<scenario>-<resolution>`) |
| `--output FILE` | Output path (default: `manifest/resource-manifest.yaml`) |
| `--namespace NS` | Kubernetes namespace (default: `mxl`) |
| `--cpu-margin FACTOR` | CPU safety multiplier (default: `1.20`) |
| `--memory-margin FACTOR` | Memory safety multiplier (default: `1.25`) |
| `--no-validate` | Skip schema validation |
| `--dry-run` | Print manifest to stdout instead of writing a file |

**Example:**

```bash
scripts/create-dmf-manifest.sh \
    --perfspect results/perfspect/worker-1/latest \
    --profile   results/pinned-exclusive-1str-20240115T120000Z \
    --service   encoder \
    --output    manifest/mxl-encoder-pinned.yaml
```

---

## Expected PerfSpect input

The generator reads a PerfSpect system configuration JSON file. The top-level
sections `CPU`, `ISA`, `DIMM`, and `Memory` may each be an array of records or
a single dictionary — both forms are supported.

```json
{
  "CPU": [{"CPU Model": "Intel(R) Xeon(R) 6767P", "Sockets": "2"}],
  "ISA": [{"AVX-512 Foundation": "Yes", "AMX-COMPLEX Instruction": "No"}],
  "DIMM": [{"Type": "DDR5", "Configured Speed": "6400MT/s"}],
  "Memory": [{"Installed Memory": "512GB (16x32GB DDR5 6400MT/s [6400MT/s])"}]
}
```



## Validation and output

By default the manifest is validated against the bundled schema at
`manifest/schema/resource_manifest_schema.yaml`.

Use `--dry-run` to print the manifest to stdout without writing a file.
Use `--no-validate` to skip validation (for offline use or troubleshooting).

Generation fails with a non-zero exit when:

* `--perfspect` is omitted or does not resolve to usable data
* No CPU features can be extracted from the PerfSpect input
* `config.json` or `metrics.csv` is missing or incomplete
