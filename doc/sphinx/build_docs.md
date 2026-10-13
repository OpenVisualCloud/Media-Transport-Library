# Build Documentation Guide

The documentation site is <https://openvisualcloud.github.io/Media-Transport-Library/>.
The [GitHub Pages workflow](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/.github/workflows/github_pages_update.yml)
builds it with the steps of this guide:

* On a pull request that changes the documentation, the workflow builds the site and does not publish it.
* On a push to `main`, the workflow builds the site and publishes it.

The build treats each Sphinx warning as an error. A pull request that adds a warning fails the check.

## 1. Prerequisites (Debian and Ubuntu)

The pinned packages need Python 3.12 or later, for example Ubuntu 24.04 or
Debian 13. Install the system packages:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends python3 python3-venv make
```

Create a Python virtual environment, and install the Python packages into it.
[requirements.txt](requirements.txt) pins the versions that the workflow uses.

```bash
cd {project_dir}
python3 -m venv .venv-docs
. .venv-docs/bin/activate
python3 -m pip install -r doc/sphinx/requirements.txt
```

> **Note:** Do not install the packages with `pip` outside a virtual environment.
> On Debian 12, Ubuntu 24.04, and later releases, `pip` refuses to change the
> system Python (PEP 668). The `python3-sphinx` package of the distribution is
> too old for these extensions.

## 2. Build documentation (HTML)

Build the HTML pages with the same checks as the workflow:

```bash
make -C doc/sphinx html SPHINXOPTS="-W --keep-going"
```

The pages are in `doc/_build/html`. For a fast build that shows the warnings but
does not fail, remove `SPHINXOPTS`:

```bash
make -C doc/sphinx html
```

To make a clean build, remove the `doc/_build` directory first.

## 3. Open built documentation (HTML)

Open `doc/_build/html/index.html` in a web browser.

### 3.1. Alternative: run a local web server

```bash
python3 -m http.server --directory doc/_build/html 8080
```

Or use nginx in Docker:

```bash
docker run -it --rm -d -p 8080:80 --name web -v ./doc/_build/html:/usr/share/nginx/html nginx
```

Open `http://<your-ip-addr>:8080/` or `http://127.0.0.1:8080/` in a web browser.

## 4. Add a page

1. Write the page in Markdown.
2. Add the page to a `toctree` in [index.rst](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/main/index.rst).
   A page that is in no `toctree` gives a warning, and the build fails.
3. To link to another page, use a relative path to its `.md` file, for example
   `[Run Guide](../run.md#3-dpdk-pmd-setup)`. The heading anchors are the same as on GitHub.
4. To link to a source file or a directory, use a relative path. On the site,
   the link goes to the same path on GitHub.

[conf.py](conf.py) excludes the files that are not user documentation: the AI
agent files (`.claude/`, `.github/`, `CLAUDE.md`), `tests/`, `patches/`, and the
parts in `doc/chunks/` that other pages include.
