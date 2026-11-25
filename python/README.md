# tickforge (python bindings)

Pybind11 bindings for the Tickforge C++ core, used only for analysis and plots.

Managed with [uv](https://github.com/astral-sh/uv) and built with
[scikit-build-core](https://github.com/scikit-build/scikit-build-core).

```bash
cd python
uv venv
uv pip install -e ".[analysis,dev]"
uv run pytest
```
