# Live loads this folder as a control surface script named Griddy. See Griddy.py.
from .Griddy import Griddy


def create_instance(c_instance):
    return Griddy(c_instance)
