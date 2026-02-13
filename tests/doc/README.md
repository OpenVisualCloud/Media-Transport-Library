# MTL Test Documentation

Sphinx documentation for MTL test suite (pytest and gtest).

## Prerequisites

```bash
pip install sphinx sphinx_rtd_theme breathe
```

For C++ docs:
```bash
sudo apt-get install doxygen  # Ubuntu/Debian
brew install doxygen          # macOS
```

## Build

```bash
cd tests/doc
make html  # Output: _build/html/index.html
```

Other formats:
```bash
make latexpdf  # Requires LaTeX
make epub
make text
```

## Structure

```text
doc/
├── conf.py              # Sphinx config
├── index.rst           # Main index
├── pytest/             # Python tests
│   ├── index.rst
│   ├── kernel_socket.rst
│   └── common.rst
└── gtest/              # C++ tests
    └── index.rst
```

## Adding Test Documentation

### Python Tests

Module docstring:
```python
"""Module description.

Detailed test information.
"""
```

Function docstring:
```python
def test_function(param1, param2):
    """Test description.

    :param param1: Description
    :type param1: str
    :param param2: Description
    :type param2: int
    """
```

Add to .rst file:
```rst
.. automodule:: path.to.module
   :members:
```

### C++ Tests

Add Doxygen comments:
```cpp
/**
 * @brief Brief description
 * @param param1 Description
 * @return Return description
 */
void test_function(int param1);
```

Generate XML:
```bash
cd tests/integration_tests && doxygen Doxyfile
```

## View

```bash
open _build/html/index.html
# Or: cd _build/html && python -m http.server 8000
```

## Configuration

In `conf.py`:
- **autodoc_mock_imports**: Mock external dependencies
- **breathe_projects**: Path to Doxygen XML
- **html_theme**: Documentation theme

## Troubleshooting

**Import errors**: Check `sys.path` in conf.py
**Missing deps**: Add to `autodoc_mock_imports`
**C++ docs missing**: Verify Doxygen XML exists and Breathe config

## CI Integration

```bash
pip install -r requirements-docs.txt
sphinx-build -W -b html doc _build/html  # -W treats warnings as errors
```
