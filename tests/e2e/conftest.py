import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib.container import IrcdContainer


@pytest.fixture
def ircd():
    c = IrcdContainer().up()
    try:
        c.wait_ready()
        yield c
    finally:
        c.down()
