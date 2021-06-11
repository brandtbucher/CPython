import array
import collections
import dataclasses
import enum
import inspect
import unittest


def no_perf(f):
    f.no_perf = None
    return f


@dataclasses.dataclass
class MyClass:
    x: int
    y: str
    __match_args__ = ("x", "y")


@dataclasses.dataclass
class Point:
    x: int
    y: int


class TestPatma(unittest.TestCase):

    def assert_syntax_error(self, code: str):
        with self.assertRaises(SyntaxError):
            compile(inspect.cleandoc(code), "<test>", "exec")

    def test_patma_000(self):
        match 0:
            case 0:
                x = True
        self.assertIs(x, True)

    def test_patma_001(self):
        match 0:
            case 0 if False:
                x = False
            case 0 if True:
                x = True
        self.assertIs(x, True)

    def test_patma_002(self):
        match 0:
            case 0:
                x = True
            case 0:
                x = False
        self.assertIs(x, True)

    def test_patma_003(self):
        x = False
        match 0:
            case 0 | 1 | 2 | 3:
                x = True
        self.assertIs(x, True)

    def test_patma_004(self):
        x = False
        match 1:
            case 0 | 1 | 2 | 3:
                x = True
        self.assertIs(x, True)

    def test_patma_005(self):
        x = False
        match 2:
            case 0 | 1 | 2 | 3:
                x = True
        self.assertIs(x, True)

    def test_patma_006(self):
        x = False
        match 3:
            case 0 | 1 | 2 | 3:
                x = True
        self.assertIs(x, True)

    def test_patma_007(self):
        x = False
        match 4:
            case 0 | 1 | 2 | 3:
                x = True
        self.assertIs(x, False)

    def test_patma_008(self):
        x = 0
        class A:
            y = 1
        match x:
            case A.y as z:
                pass
        self.assertEqual(x, 0)
        self.assertEqual(A.y, 1)

    def test_patma_009(self):
        class A:
            B = 0
        match 0:
            case x if x:
                z = 0
            case _ as y if y == x and y:
                z = 1
            case A.B:
                z = 2
        self.assertEqual(A.B, 0)
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertEqual(z, 2)

    def test_patma_010(self):
        match ():
            case []:
                x = 0
        self.assertEqual(x, 0)

    def test_patma_011(self):
        match (0, 1, 2):
            case [*x]:
                y = 0
        self.assertEqual(x, [0, 1, 2])
        self.assertEqual(y, 0)

    def test_patma_012(self):
        match (0, 1, 2):
            case [0, *x]:
                y = 0
        self.assertEqual(x, [1, 2])
        self.assertEqual(y, 0)

    def test_patma_013(self):
        match (0, 1, 2):
            case [0, 1, *x,]:
                y = 0
        self.assertEqual(x, [2])
        self.assertEqual(y, 0)

    def test_patma_014(self):
        match (0, 1, 2):
            case [0, 1, 2, *x]:
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_015(self):
        match (0, 1, 2):
            case [*x, 2,]:
                y = 0
        self.assertEqual(x, [0, 1])
        self.assertEqual(y, 0)

    def test_patma_016(self):
        match (0, 1, 2):
            case [*x, 1, 2]:
                y = 0
        self.assertEqual(x, [0])
        self.assertEqual(y, 0)

    def test_patma_017(self):
        match (0, 1, 2):
            case [*x, 0, 1, 2,]:
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_018(self):
        match (0, 1, 2):
            case [0, *x, 2]:
                y = 0
        self.assertEqual(x, [1])
        self.assertEqual(y, 0)

    def test_patma_019(self):
        match (0, 1, 2):
            case [0, 1, *x, 2,]:
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_020(self):
        match (0, 1, 2):
            case [0, *x, 1, 2]:
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_021(self):
        match (0, 1, 2):
            case [*x,]:
                y = 0
        self.assertEqual(x, [0, 1, 2])
        self.assertEqual(y, 0)

    def test_patma_022(self):
        x = {}
        match x:
            case {}:
                y = 0
        self.assertEqual(x, {})
        self.assertEqual(y, 0)

    def test_patma_023(self):
        x = {0: 0}
        match x:
            case {}:
                y = 0
        self.assertEqual(x, {0: 0})
        self.assertEqual(y, 0)

    def test_patma_024(self):
        x = {}
        y = None
        match x:
            case {0: 0}:
                y = 0
        self.assertEqual(x, {})
        self.assertIs(y, None)

    def test_patma_025(self):
        x = {0: 0}
        match x:
            case {0: (0 | 1 | 2 as z)}:
                y = 0
        self.assertEqual(x, {0: 0})
        self.assertEqual(y, 0)
        self.assertEqual(z, 0)

    def test_patma_026(self):
        x = {0: 1}
        match x:
            case {0: (0 | 1 | 2 as z)}:
                y = 0
        self.assertEqual(x, {0: 1})
        self.assertEqual(y, 0)
        self.assertEqual(z, 1)

    def test_patma_027(self):
        x = {0: 2}
        match x:
            case {0: (0 | 1 | 2 as z)}:
                y = 0
        self.assertEqual(x, {0: 2})
        self.assertEqual(y, 0)
        self.assertEqual(z, 2)

    def test_patma_028(self):
        x = {0: 3}
        y = None
        match x:
            case {0: (0 | 1 | 2 as z)}:
                y = 0
        self.assertEqual(x, {0: 3})
        self.assertIs(y, None)

    def test_patma_029(self):
        x = {}
        y = None
        match x:
            case {0: [1, 2, {}]}:
                y = 0
            case {0: [1, 2, {}], 1: [[]]}:
                y = 1
            case []:
                y = 2
        self.assertEqual(x, {})
        self.assertIs(y, None)

    def test_patma_030(self):
        x = {False: (True, 2.0, {})}
        match x:
            case {0: [1, 2, {}]}:
                y = 0
            case {0: [1, 2, {}], 1: [[]]}:
                y = 1
            case []:
                y = 2
        self.assertEqual(x, {False: (True, 2.0, {})})
        self.assertEqual(y, 0)

    def test_patma_031(self):
        x = {False: (True, 2.0, {}), 1: [[]], 2: 0}
        match x:
            case {0: [1, 2, {}]}:
                y = 0
            case {0: [1, 2, {}], 1: [[]]}:
                y = 1
            case []:
                y = 2
        self.assertEqual(x, {False: (True, 2.0, {}), 1: [[]], 2: 0})
        self.assertEqual(y, 0)

    def test_patma_032(self):
        x = {False: (True, 2.0, {}), 1: [[]], 2: 0}
        match x:
            case {0: [1, 2]}:
                y = 0
            case {0: [1, 2, {}], 1: [[]]}:
                y = 1
            case []:
                y = 2
        self.assertEqual(x, {False: (True, 2.0, {}), 1: [[]], 2: 0})
        self.assertEqual(y, 1)

    def test_patma_033(self):
        x = []
        match x:
            case {0: [1, 2, {}]}:
                y = 0
            case {0: [1, 2, {}], 1: [[]]}:
                y = 1
            case []:
                y = 2
        self.assertEqual(x, [])
        self.assertEqual(y, 2)

    def test_patma_034(self):
        x = {0: 0}
        match x:
            case {0: [1, 2, {}]}:
                y = 0
            case {0: ([1, 2, {}] | False)} | {1: [[]]} | {0: [1, 2, {}]} | [] | "X" | {}:
                y = 1
            case []:
                y = 2
        self.assertEqual(x, {0: 0})
        self.assertEqual(y, 1)

    def test_patma_035(self):
        x = {0: 0}
        match x:
            case {0: [1, 2, {}]}:
                y = 0
            case {0: [1, 2, {}] | True} | {1: [[]]} | {0: [1, 2, {}]} | [] | "X" | {}:
                y = 1
            case []:
                y = 2
        self.assertEqual(x, {0: 0})
        self.assertEqual(y, 1)

    def test_patma_036(self):
        x = 0
        match x:
            case 0 | 1 | 2:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_037(self):
        x = 1
        match x:
            case 0 | 1 | 2:
                y = 0
        self.assertEqual(x, 1)
        self.assertEqual(y, 0)

    def test_patma_038(self):
        x = 2
        match x:
            case 0 | 1 | 2:
                y = 0
        self.assertEqual(x, 2)
        self.assertEqual(y, 0)

    def test_patma_039(self):
        x = 3
        y = None
        match x:
            case 0 | 1 | 2:
                y = 0
        self.assertEqual(x, 3)
        self.assertIs(y, None)

    def test_patma_040(self):
        x = 0
        match x:
            case (0 as z) | (1 as z) | (2 as z) if z == x % 2:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertEqual(z, 0)

    def test_patma_041(self):
        x = 1
        match x:
            case (0 as z) | (1 as z) | (2 as z) if z == x % 2:
                y = 0
        self.assertEqual(x, 1)
        self.assertEqual(y, 0)
        self.assertEqual(z, 1)

    def test_patma_042(self):
        x = 2
        y = None
        match x:
            case (0 as z) | (1 as z) | (2 as z) if z == x % 2:
                y = 0
        self.assertEqual(x, 2)
        self.assertIs(y, None)
        self.assertEqual(z, 2)

    def test_patma_043(self):
        x = 3
        y = None
        match x:
            case (0 as z) | (1 as z) | (2 as z) if z == x % 2:
                y = 0
        self.assertEqual(x, 3)
        self.assertIs(y, None)

    def test_patma_044(self):
        x = ()
        match x:
            case []:
                y = 0
        self.assertEqual(x, ())
        self.assertEqual(y, 0)

    def test_patma_045(self):
        x = ()
        match x:
            case ():
                y = 0
        self.assertEqual(x, ())
        self.assertEqual(y, 0)

    def test_patma_046(self):
        x = (0,)
        match x:
            case [0]:
                y = 0
        self.assertEqual(x, (0,))
        self.assertEqual(y, 0)

    def test_patma_047(self):
        x = ((),)
        match x:
            case [[]]:
                y = 0
        self.assertEqual(x, ((),))
        self.assertEqual(y, 0)

    def test_patma_048(self):
        x = [0, 1]
        match x:
            case [0, 1] | [1, 0]:
                y = 0
        self.assertEqual(x, [0, 1])
        self.assertEqual(y, 0)

    def test_patma_049(self):
        x = [1, 0]
        match x:
            case [0, 1] | [1, 0]:
                y = 0
        self.assertEqual(x, [1, 0])
        self.assertEqual(y, 0)

    def test_patma_050(self):
        x = [0, 0]
        y = None
        match x:
            case [0, 1] | [1, 0]:
                y = 0
        self.assertEqual(x, [0, 0])
        self.assertIs(y, None)

    def test_patma_051(self):
        w = None
        x = [1, 0]
        match x:
            case [(0 as w)]:
                y = 0
            case [z] | [1, (0 | 1 as z)] | [z]:
                y = 1
        self.assertIs(w, None)
        self.assertEqual(x, [1, 0])
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)

    def test_patma_052(self):
        x = [1, 0]
        match x:
            case [0]:
                y = 0
            case [1, 0] if (x := x[:0]):
                y = 1
            case [1, 0]:
                y = 2
        self.assertEqual(x, [])
        self.assertEqual(y, 2)

    def test_patma_053(self):
        x = {0}
        y = None
        match x:
            case [0]:
                y = 0
        self.assertEqual(x, {0})
        self.assertIs(y, None)

    def test_patma_054(self):
        x = set()
        y = None
        match x:
            case []:
                y = 0
        self.assertEqual(x, set())
        self.assertIs(y, None)

    def test_patma_055(self):
        x = iter([1, 2, 3])
        y = None
        match x:
            case []:
                y = 0
        self.assertEqual([*x], [1, 2, 3])
        self.assertIs(y, None)

    def test_patma_056(self):
        x = {}
        y = None
        match x:
            case []:
                y = 0
        self.assertEqual(x, {})
        self.assertIs(y, None)

    def test_patma_057(self):
        x = {0: False, 1: True}
        y = None
        match x:
            case [0, 1]:
                y = 0
        self.assertEqual(x, {0: False, 1: True})
        self.assertIs(y, None)

    def test_patma_058(self):
        x = 0
        match x:
            case 0:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_059(self):
        x = 0
        y = None
        match x:
            case False:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, None)

    def test_patma_060(self):
        x = 0
        y = None
        match x:
            case 1:
                y = 0
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_061(self):
        x = 0
        y = None
        match x:
            case None:
                y = 0
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_062(self):
        x = 0
        match x:
            case 0:
                y = 0
            case 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_063(self):
        x = 0
        y = None
        match x:
            case 1:
                y = 0
            case 1:
                y = 1
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_064(self):
        x = "x"
        match x:
            case "x":
                y = 0
            case "y":
                y = 1
        self.assertEqual(x, "x")
        self.assertEqual(y, 0)

    def test_patma_065(self):
        x = "x"
        match x:
            case "y":
                y = 0
            case "x":
                y = 1
        self.assertEqual(x, "x")
        self.assertEqual(y, 1)

    def test_patma_066(self):
        x = "x"
        match x:
            case "":
                y = 0
            case "x":
                y = 1
        self.assertEqual(x, "x")
        self.assertEqual(y, 1)

    def test_patma_067(self):
        x = b"x"
        match x:
            case b"y":
                y = 0
            case b"x":
                y = 1
        self.assertEqual(x, b"x")
        self.assertEqual(y, 1)

    def test_patma_068(self):
        x = 0
        match x:
            case 0 if False:
                y = 0
            case 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)

    def test_patma_069(self):
        x = 0
        y = None
        match x:
            case 0 if 0:
                y = 0
            case 0 if 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_070(self):
        x = 0
        match x:
            case 0 if True:
                y = 0
            case 0 if True:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_071(self):
        x = 0
        match x:
            case 0 if 1:
                y = 0
            case 0 if 1:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_072(self):
        x = 0
        match x:
            case 0 if True:
                y = 0
            case 0 if True:
                y = 1
        y = 2
        self.assertEqual(x, 0)
        self.assertEqual(y, 2)

    def test_patma_073(self):
        x = 0
        match x:
            case 0 if 0:
                y = 0
            case 0 if 1:
                y = 1
        y = 2
        self.assertEqual(x, 0)
        self.assertEqual(y, 2)

    def test_patma_074(self):
        x = 0
        y = None
        match x:
            case 0 if not (x := 1):
                y = 0
            case 1:
                y = 1
        self.assertEqual(x, 1)
        self.assertIs(y, None)

    def test_patma_075(self):
        x = "x"
        match x:
            case ["x"]:
                y = 0
            case "x":
                y = 1
        self.assertEqual(x, "x")
        self.assertEqual(y, 1)

    def test_patma_076(self):
        x = b"x"
        match x:
            case [b"x"]:
                y = 0
            case ["x"]:
                y = 1
            case [120]:
                y = 2
            case b"x":
                y = 4
        self.assertEqual(x, b"x")
        self.assertEqual(y, 4)

    def test_patma_077(self):
        x = bytearray(b"x")
        y = None
        match x:
            case [120]:
                y = 0
            case 120:
                y = 1
        self.assertEqual(x, b"x")
        self.assertIs(y, None)

    def test_patma_078(self):
        x = ""
        match x:
            case []:
                y = 0
            case [""]:
                y = 1
            case "":
                y = 2
        self.assertEqual(x, "")
        self.assertEqual(y, 2)

    def test_patma_079(self):
        x = "xxx"
        match x:
            case ["x", "x", "x"]:
                y = 0
            case ["xxx"]:
                y = 1
            case "xxx":
                y = 2
        self.assertEqual(x, "xxx")
        self.assertEqual(y, 2)

    def test_patma_080(self):
        x = b"xxx"
        match x:
            case [120, 120, 120]:
                y = 0
            case [b"xxx"]:
                y = 1
            case b"xxx":
                y = 2
        self.assertEqual(x, b"xxx")
        self.assertEqual(y, 2)

    def test_patma_081(self):
        x = 0
        match x:
            case 0 if not (x := 1):
                y = 0
            case (0 as z):
                y = 1
        self.assertEqual(x, 1)
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)

    def test_patma_082(self):
        x = 0
        match x:
            case (1 as z) if not (x := 1):
                y = 0
            case 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)

    def test_patma_083(self):
        x = 0
        match x:
            case (0 as z):
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertEqual(z, 0)

    def test_patma_084(self):
        x = 0
        y = None
        match x:
            case (1 as z):
                y = 0
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_085(self):
        x = 0
        y = None
        match x:
            case (0 as z) if (w := 0):
                y = 0
        self.assertEqual(w, 0)
        self.assertEqual(x, 0)
        self.assertIs(y, None)
        self.assertEqual(z, 0)

    def test_patma_086(self):
        x = 0
        match x:
            case ((0 as w) as z):
                y = 0
        self.assertEqual(w, 0)
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertEqual(z, 0)

    def test_patma_087(self):
        x = 0
        match x:
            case (0 | 1) | 2:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_088(self):
        x = 1
        match x:
            case (0 | 1) | 2:
                y = 0
        self.assertEqual(x, 1)
        self.assertEqual(y, 0)

    def test_patma_089(self):
        x = 2
        match x:
            case (0 | 1) | 2:
                y = 0
        self.assertEqual(x, 2)
        self.assertEqual(y, 0)

    def test_patma_090(self):
        x = 3
        y = None
        match x:
            case (0 | 1) | 2:
                y = 0
        self.assertEqual(x, 3)
        self.assertIs(y, None)

    def test_patma_091(self):
        x = 0
        match x:
            case 0 | (1 | 2):
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_092(self):
        x = 1
        match x:
            case 0 | (1 | 2):
                y = 0
        self.assertEqual(x, 1)
        self.assertEqual(y, 0)

    def test_patma_093(self):
        x = 2
        match x:
            case 0 | (1 | 2):
                y = 0
        self.assertEqual(x, 2)
        self.assertEqual(y, 0)

    def test_patma_094(self):
        x = 3
        y = None
        match x:
            case 0 | (1 | 2):
                y = 0
        self.assertEqual(x, 3)
        self.assertIs(y, None)

    def test_patma_095(self):
        x = 0
        match x:
            case -0:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_096(self):
        x = 0
        match x:
            case -0.0:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_097(self):
        x = 0
        match x:
            case -0j:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_098(self):
        x = 0
        match x:
            case -0.0j:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_099(self):
        x = -1
        match x:
            case -1:
                y = 0
        self.assertEqual(x, -1)
        self.assertEqual(y, 0)

    def test_patma_100(self):
        x = -1.5
        match x:
            case -1.5:
                y = 0
        self.assertEqual(x, -1.5)
        self.assertEqual(y, 0)

    def test_patma_101(self):
        x = -1j
        match x:
            case -1j:
                y = 0
        self.assertEqual(x, -1j)
        self.assertEqual(y, 0)

    def test_patma_102(self):
        x = -1.5j
        match x:
            case -1.5j:
                y = 0
        self.assertEqual(x, -1.5j)
        self.assertEqual(y, 0)

    def test_patma_103(self):
        x = 0
        match x:
            case 0 + 0j:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_104(self):
        x = 0
        match x:
            case 0 - 0j:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_105(self):
        x = 0
        match x:
            case -0 + 0j:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_106(self):
        x = 0
        match x:
            case -0 - 0j:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_107(self):
        x = 0.25 + 1.75j
        match x:
            case 0.25 + 1.75j:
                y = 0
        self.assertEqual(x, 0.25 + 1.75j)
        self.assertEqual(y, 0)

    def test_patma_108(self):
        x = 0.25 - 1.75j
        match x:
            case 0.25 - 1.75j:
                y = 0
        self.assertEqual(x, 0.25 - 1.75j)
        self.assertEqual(y, 0)

    def test_patma_109(self):
        x = -0.25 + 1.75j
        match x:
            case -0.25 + 1.75j:
                y = 0
        self.assertEqual(x, -0.25 + 1.75j)
        self.assertEqual(y, 0)

    def test_patma_110(self):
        x = -0.25 - 1.75j
        match x:
            case -0.25 - 1.75j:
                y = 0
        self.assertEqual(x, -0.25 - 1.75j)
        self.assertEqual(y, 0)

    def test_patma_111(self):
        class A:
            B = 0
        x = 0
        match x:
            case A.B:
                y = 0
        self.assertEqual(A.B, 0)
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_112(self):
        class A:
            class B:
                C = 0
        x = 0
        match x:
            case A.B.C:
                y = 0
        self.assertEqual(A.B.C, 0)
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_113(self):
        class A:
            class B:
                C = 0
                D = 1
        x = 1
        match x:
            case A.B.C:
                y = 0
            case A.B.D:
                y = 1
        self.assertEqual(A.B.C, 0)
        self.assertEqual(A.B.D, 1)
        self.assertEqual(x, 1)
        self.assertEqual(y, 1)

    def test_patma_114(self):
        class A:
            class B:
                class C:
                    D = 0
        x = 0
        match x:
            case A.B.C.D:
                y = 0
        self.assertEqual(A.B.C.D, 0)
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_115(self):
        class A:
            class B:
                class C:
                    D = 0
                    E = 1
        x = 1
        match x:
            case A.B.C.D:
                y = 0
            case A.B.C.E:
                y = 1
        self.assertEqual(A.B.C.D, 0)
        self.assertEqual(A.B.C.E, 1)
        self.assertEqual(x, 1)
        self.assertEqual(y, 1)

    def test_patma_116(self):
        match = case = 0
        match match:
            case case:
                x = 0
        self.assertEqual(match, 0)
        self.assertEqual(case, 0)
        self.assertEqual(x, 0)

    def test_patma_117(self):
        match = case = 0
        match case:
            case match:
                x = 0
        self.assertEqual(match, 0)
        self.assertEqual(case, 0)
        self.assertEqual(x, 0)

    def test_patma_118(self):
        x = []
        match x:
            case [*_, _]:
                y = 0
            case []:
                y = 1
        self.assertEqual(x, [])
        self.assertEqual(y, 1)

    def test_patma_119(self):
        x = collections.defaultdict(int)
        match x:
            case {0: 0}:
                y = 0
            case {}:
                y = 1
        self.assertEqual(x, {})
        self.assertEqual(y, 1)

    def test_patma_120(self):
        x = collections.defaultdict(int)
        match x:
            case {0: 0}:
                y = 0
            case {**z}:
                y = 1
        self.assertEqual(x, {})
        self.assertEqual(y, 1)
        self.assertEqual(z, {})

    def test_patma_121(self):
        match ():
            case ():
                x = 0
        self.assertEqual(x, 0)

    def test_patma_122(self):
        match (0, 1, 2):
            case (*x,):
                y = 0
        self.assertEqual(x, [0, 1, 2])
        self.assertEqual(y, 0)

    def test_patma_123(self):
        match (0, 1, 2):
            case 0, *x:
                y = 0
        self.assertEqual(x, [1, 2])
        self.assertEqual(y, 0)

    def test_patma_124(self):
        match (0, 1, 2):
            case (0, 1, *x,):
                y = 0
        self.assertEqual(x, [2])
        self.assertEqual(y, 0)

    def test_patma_125(self):
        match (0, 1, 2):
            case 0, 1, 2, *x:
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_126(self):
        match (0, 1, 2):
            case *x, 2,:
                y = 0
        self.assertEqual(x, [0, 1])
        self.assertEqual(y, 0)

    def test_patma_127(self):
        match (0, 1, 2):
            case (*x, 1, 2):
                y = 0
        self.assertEqual(x, [0])
        self.assertEqual(y, 0)

    def test_patma_128(self):
        match (0, 1, 2):
            case *x, 0, 1, 2,:
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_129(self):
        match (0, 1, 2):
            case (0, *x, 2):
                y = 0
        self.assertEqual(x, [1])
        self.assertEqual(y, 0)

    def test_patma_130(self):
        match (0, 1, 2):
            case 0, 1, *x, 2,:
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_131(self):
        match (0, 1, 2):
            case (0, *x, 1, 2):
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)

    def test_patma_132(self):
        match (0, 1, 2):
            case *x,:
                y = 0
        self.assertEqual(x, [0, 1, 2])
        self.assertEqual(y, 0)

    def test_patma_133(self):
        x = collections.defaultdict(int, {0: 1})
        match x:
            case {1: 0}:
                y = 0
            case {0: 0}:
                y = 1
            case {}:
                y = 2
        self.assertEqual(x, {0: 1})
        self.assertEqual(y, 2)

    def test_patma_134(self):
        x = collections.defaultdict(int, {0: 1})
        match x:
            case {1: 0}:
                y = 0
            case {0: 0}:
                y = 1
            case {**z}:
                y = 2
        self.assertEqual(x, {0: 1})
        self.assertEqual(y, 2)
        self.assertEqual(z, {0: 1})

    def test_patma_135(self):
        x = collections.defaultdict(int, {0: 1})
        match x:
            case {1: 0}:
                y = 0
            case {0: 0}:
                y = 1
            case {0: _, **z}:
                y = 2
        self.assertEqual(x, {0: 1})
        self.assertEqual(y, 2)
        self.assertEqual(z, {})

    def test_patma_136(self):
        x = {0: 1}
        match x:
            case {1: 0}:
                y = 0
            case {0: 0}:
                y = 0
            case {}:
                y = 1
        self.assertEqual(x, {0: 1})
        self.assertEqual(y, 1)

    def test_patma_137(self):
        x = {0: 1}
        match x:
            case {1: 0}:
                y = 0
            case {0: 0}:
                y = 0
            case {**z}:
                y = 1
        self.assertEqual(x, {0: 1})
        self.assertEqual(y, 1)
        self.assertEqual(z, {0: 1})

    def test_patma_138(self):
        x = {0: 1}
        match x:
            case {1: 0}:
                y = 0
            case {0: 0}:
                y = 0
            case {0: _, **z}:
                y = 1
        self.assertEqual(x, {0: 1})
        self.assertEqual(y, 1)
        self.assertEqual(z, {})

    def test_patma_139(self):
        x = False
        match x:
            case bool(z):
                y = 0
        self.assertIs(x, False)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_140(self):
        x = True
        match x:
            case bool(z):
                y = 0
        self.assertIs(x, True)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_141(self):
        x = bytearray()
        match x:
            case bytearray(z):
                y = 0
        self.assertEqual(x, bytearray())
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_142(self):
        x = b""
        match x:
            case bytes(z):
                y = 0
        self.assertEqual(x, b"")
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_143(self):
        x = {}
        match x:
            case dict(z):
                y = 0
        self.assertEqual(x, {})
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_144(self):
        x = 0.0
        match x:
            case float(z):
                y = 0
        self.assertEqual(x, 0.0)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_145(self):
        x = frozenset()
        match x:
            case frozenset(z):
                y = 0
        self.assertEqual(x, frozenset())
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_146(self):
        x = 0
        match x:
            case int(z):
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_147(self):
        x = []
        match x:
            case list(z):
                y = 0
        self.assertEqual(x, [])
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_148(self):
        x = set()
        match x:
            case set(z):
                y = 0
        self.assertEqual(x, set())
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_149(self):
        x = ""
        match x:
            case str(z):
                y = 0
        self.assertEqual(x, "")
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_150(self):
        x = ()
        match x:
            case tuple(z):
                y = 0
        self.assertEqual(x, ())
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_151(self):
        x = 0
        match x,:
            case y,:
                z = 0
        self.assertEqual(x, 0)
        self.assertIs(y, x)
        self.assertIs(z, 0)

    def test_patma_152(self):
        w = 0
        x = 0
        match w, x:
            case y, z:
                v = 0
        self.assertEqual(w, 0)
        self.assertEqual(x, 0)
        self.assertIs(y, w)
        self.assertIs(z, x)
        self.assertEqual(v, 0)

    def test_patma_153(self):
        x = 0
        match w := x,:
            case y as v,:
                z = 0
        self.assertEqual(x, 0)
        self.assertIs(y, x)
        self.assertEqual(z, 0)
        self.assertIs(w, x)
        self.assertIs(v, y)

    def test_patma_154(self):
        x = 0
        y = None
        match x:
            case 0 if x:
                y = 0
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_155(self):
        x = 0
        y = None
        match x:
            case 1e1000:
                y = 0
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_156(self):
        x = 0
        match x:
            case z:
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_157(self):
        x = 0
        y = None
        match x:
            case _ if x:
                y = 0
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_158(self):
        x = 0
        match x:
            case -1e1000:
                y = 0
            case 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)

    def test_patma_159(self):
        x = 0
        match x:
            case 0 if not x:
                y = 0
            case 1:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_160(self):
        x = 0
        z = None
        match x:
            case 0:
                y = 0
            case z if x:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, None)

    def test_patma_161(self):
        x = 0
        match x:
            case 0:
                y = 0
            case _:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_162(self):
        x = 0
        match x:
            case 1 if x:
                y = 0
            case 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)

    def test_patma_163(self):
        x = 0
        y = None
        match x:
            case 1:
                y = 0
            case 1 if not x:
                y = 1
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_164(self):
        x = 0
        match x:
            case 1:
                y = 0
            case z:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)
        self.assertIs(z, x)

    def test_patma_165(self):
        x = 0
        match x:
            case 1 if x:
                y = 0
            case _:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)

    def test_patma_166(self):
        x = 0
        match x:
            case z if not z:
                y = 0
            case 0 if x:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_167(self):
        x = 0
        match x:
            case z if not z:
                y = 0
            case 1:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_168(self):
        x = 0
        match x:
            case z if not x:
                y = 0
            case z:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_169(self):
        x = 0
        match x:
            case z if not z:
                y = 0
            case _ if x:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, x)

    def test_patma_170(self):
        x = 0
        match x:
            case _ if not x:
                y = 0
            case 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_171(self):
        x = 0
        y = None
        match x:
            case _ if x:
                y = 0
            case 1:
                y = 1
        self.assertEqual(x, 0)
        self.assertIs(y, None)

    def test_patma_172(self):
        x = 0
        z = None
        match x:
            case _ if not x:
                y = 0
            case z if not x:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertIs(z, None)

    def test_patma_173(self):
        x = 0
        match x:
            case _ if not x:
                y = 0
            case _:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_174(self):
        def http_error(status):
            match status:
                case 400:
                    return "Bad request"
                case 401:
                    return "Unauthorized"
                case 403:
                    return "Forbidden"
                case 404:
                    return "Not found"
                case 418:
                    return "I'm a teapot"
                case _:
                    return "Something else"
        self.assertEqual(http_error(400), "Bad request")
        self.assertEqual(http_error(401), "Unauthorized")
        self.assertEqual(http_error(403), "Forbidden")
        self.assertEqual(http_error(404), "Not found")
        self.assertEqual(http_error(418), "I'm a teapot")
        self.assertEqual(http_error(123), "Something else")
        self.assertEqual(http_error("400"), "Something else")
        self.assertEqual(http_error(401 | 403 | 404), "Something else")  # 407

    def test_patma_175(self):
        def http_error(status):
            match status:
                case 400:
                    return "Bad request"
                case 401 | 403 | 404:
                    return "Not allowed"
                case 418:
                    return "I'm a teapot"
        self.assertEqual(http_error(400), "Bad request")
        self.assertEqual(http_error(401), "Not allowed")
        self.assertEqual(http_error(403), "Not allowed")
        self.assertEqual(http_error(404), "Not allowed")
        self.assertEqual(http_error(418), "I'm a teapot")
        self.assertIs(http_error(123), None)
        self.assertIs(http_error("400"), None)
        self.assertIs(http_error(401 | 403 | 404), None)  # 407

    @no_perf
    def test_patma_176(self):
        def whereis(point):
            match point:
                case (0, 0):
                    return "Origin"
                case (0, y):
                    return f"Y={y}"
                case (x, 0):
                    return f"X={x}"
                case (x, y):
                    return f"X={x}, Y={y}"
                case _:
                    raise ValueError("Not a point")
        self.assertEqual(whereis((0, 0)), "Origin")
        self.assertEqual(whereis((0, -1.0)), "Y=-1.0")
        self.assertEqual(whereis(("X", 0)), "X=X")
        self.assertEqual(whereis((None, 1j)), "X=None, Y=1j")
        with self.assertRaises(ValueError):
            whereis(42)

    def test_patma_177(self):
        def whereis(point):
            match point:
                case Point(0, 0):
                    return "Origin"
                case Point(0, y):
                    return f"Y={y}"
                case Point(x, 0):
                    return f"X={x}"
                case Point():
                    return "Somewhere else"
                case _:
                    return "Not a point"
        self.assertEqual(whereis(Point(1, 0)), "X=1")
        self.assertEqual(whereis(Point(0, 0)), "Origin")
        self.assertEqual(whereis(10), "Not a point")
        self.assertEqual(whereis(Point(False, False)), "Origin")
        self.assertEqual(whereis(Point(0, -1.0)), "Y=-1.0")
        self.assertEqual(whereis(Point("X", 0)), "X=X")
        self.assertEqual(whereis(Point(None, 1j)), "Somewhere else")
        self.assertEqual(whereis(Point), "Not a point")
        self.assertEqual(whereis(42), "Not a point")

    def test_patma_178(self):
        def whereis(point):
            match point:
                case Point(1, var):
                    return var
        self.assertEqual(whereis(Point(1, 0)), 0)
        self.assertIs(whereis(Point(0, 0)), None)

    def test_patma_179(self):
        def whereis(point):
            match point:
                case Point(1, y=var):
                    return var
        self.assertEqual(whereis(Point(1, 0)), 0)
        self.assertIs(whereis(Point(0, 0)), None)

    def test_patma_180(self):
        def whereis(point):
            match point:
                case Point(x=1, y=var):
                    return var
        self.assertEqual(whereis(Point(1, 0)), 0)
        self.assertIs(whereis(Point(0, 0)), None)

    def test_patma_181(self):
        def whereis(point):
            match point:
                case Point(y=var, x=1):
                    return var
        self.assertEqual(whereis(Point(1, 0)), 0)
        self.assertIs(whereis(Point(0, 0)), None)

    def test_patma_182(self):
        def whereis(points):
            match points:
                case []:
                    return "No points"
                case [Point(0, 0)]:
                    return "The origin"
                case [Point(x, y)]:
                    return f"Single point {x}, {y}"
                case [Point(0, y1), Point(0, y2)]:
                    return f"Two on the Y axis at {y1}, {y2}"
                case _:
                    return "Something else"
        self.assertEqual(whereis([]), "No points")
        self.assertEqual(whereis([Point(0, 0)]), "The origin")
        self.assertEqual(whereis([Point(0, 1)]), "Single point 0, 1")
        self.assertEqual(whereis([Point(0, 0), Point(0, 0)]), "Two on the Y axis at 0, 0")
        self.assertEqual(whereis([Point(0, 1), Point(0, 1)]), "Two on the Y axis at 1, 1")
        self.assertEqual(whereis([Point(0, 0), Point(1, 0)]), "Something else")
        self.assertEqual(whereis([Point(0, 0), Point(0, 0), Point(0, 0)]), "Something else")
        self.assertEqual(whereis([Point(0, 1), Point(0, 1), Point(0, 1)]), "Something else")

    def test_patma_183(self):
        def whereis(point):
            match point:
                case Point(x, y) if x == y:
                    return f"Y=X at {x}"
                case Point(x, y):
                    return "Not on the diagonal"
        self.assertEqual(whereis(Point(0, 0)), "Y=X at 0")
        self.assertEqual(whereis(Point(0, False)), "Y=X at 0")
        self.assertEqual(whereis(Point(False, 0)), "Y=X at False")
        self.assertEqual(whereis(Point(-1 - 1j, -1 - 1j)), "Y=X at (-1-1j)")
        self.assertEqual(whereis(Point("X", "X")), "Y=X at X")
        self.assertEqual(whereis(Point("X", "x")), "Not on the diagonal")

    def test_patma_184(self):
        class Seq(collections.abc.Sequence):
            __getitem__ = None
            def __len__(self):
                return 0
        match Seq():
            case []:
                y = 0
        self.assertEqual(y, 0)

    def test_patma_185(self):
        class Seq(collections.abc.Sequence):
            __getitem__ = None
            def __len__(self):
                return 42
        match Seq():
            case [*_]:
                y = 0
        self.assertEqual(y, 0)

    def test_patma_186(self):
        class Seq(collections.abc.Sequence):
            def __getitem__(self, i):
                return i
            def __len__(self):
                return 42
        match Seq():
            case [x, *_, y]:
                z = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 41)
        self.assertEqual(z, 0)

    def test_patma_187(self):
        w = range(10)
        match w:
            case [x, y, *rest]:
                z = 0
        self.assertEqual(w, range(10))
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)
        self.assertEqual(rest, list(range(2, 10)))

    def test_patma_188(self):
        w = range(100)
        match w:
            case (x, y, *rest):
                z = 0
        self.assertEqual(w, range(100))
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)
        self.assertEqual(rest, list(range(2, 100)))

    def test_patma_189(self):
        w = range(1000)
        match w:
            case x, y, *rest:
                z = 0
        self.assertEqual(w, range(1000))
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)
        self.assertEqual(rest, list(range(2, 1000)))

    def test_patma_190(self):
        w = range(1 << 10)
        match w:
            case [x, y, *_]:
                z = 0
        self.assertEqual(w, range(1 << 10))
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)

    def test_patma_191(self):
        w = range(1 << 20)
        match w:
            case (x, y, *_):
                z = 0
        self.assertEqual(w, range(1 << 20))
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)

    def test_patma_192(self):
        w = range(1 << 30)
        match w:
            case x, y, *_:
                z = 0
        self.assertEqual(w, range(1 << 30))
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)

    def test_patma_193(self):
        x = {"bandwidth": 0, "latency": 1}
        match x:
            case {"bandwidth": b, "latency": l}:
                y = 0
        self.assertEqual(x, {"bandwidth": 0, "latency": 1})
        self.assertIs(b, x["bandwidth"])
        self.assertIs(l, x["latency"])
        self.assertEqual(y, 0)

    def test_patma_194(self):
        x = {"bandwidth": 0, "latency": 1, "key": "value"}
        match x:
            case {"latency": l, "bandwidth": b}:
                y = 0
        self.assertEqual(x, {"bandwidth": 0, "latency": 1, "key": "value"})
        self.assertIs(l, x["latency"])
        self.assertIs(b, x["bandwidth"])
        self.assertEqual(y, 0)

    def test_patma_195(self):
        x = {"bandwidth": 0, "latency": 1, "key": "value"}
        match x:
            case {"bandwidth": b, "latency": l, **rest}:
                y = 0
        self.assertEqual(x, {"bandwidth": 0, "latency": 1, "key": "value"})
        self.assertIs(b, x["bandwidth"])
        self.assertIs(l, x["latency"])
        self.assertEqual(rest, {"key": "value"})
        self.assertEqual(y, 0)

    def test_patma_196(self):
        x = {"bandwidth": 0, "latency": 1}
        match x:
            case {"latency": l, "bandwidth": b, **rest}:
                y = 0
        self.assertEqual(x, {"bandwidth": 0, "latency": 1})
        self.assertIs(l, x["latency"])
        self.assertIs(b, x["bandwidth"])
        self.assertEqual(rest, {})
        self.assertEqual(y, 0)

    def test_patma_197(self):
        w = [Point(-1, 0), Point(1, 2)]
        match w:
            case (Point(x1, y1), Point(x2, y2) as p2):
                z = 0
        self.assertEqual(w, [Point(-1, 0), Point(1, 2)])
        self.assertIs(x1, w[0].x)
        self.assertIs(y1, w[0].y)
        self.assertIs(p2, w[1])
        self.assertIs(x2, w[1].x)
        self.assertIs(y2, w[1].y)
        self.assertIs(z, 0)

    def test_patma_198(self):
        class Color(enum.Enum):
            RED = 0
            GREEN = 1
            BLUE = 2
        def f(color):
            match color:
                case Color.RED:
                    return "I see red!"
                case Color.GREEN:
                    return "Grass is green"
                case Color.BLUE:
                    return "I'm feeling the blues :("
        self.assertEqual(f(Color.RED), "I see red!")
        self.assertEqual(f(Color.GREEN), "Grass is green")
        self.assertEqual(f(Color.BLUE), "I'm feeling the blues :(")
        self.assertIs(f(Color), None)
        self.assertIs(f(0), None)
        self.assertIs(f(1), None)
        self.assertIs(f(2), None)
        self.assertIs(f(3), None)
        self.assertIs(f(False), None)
        self.assertIs(f(True), None)
        self.assertIs(f(2+0j), None)
        self.assertIs(f(3.0), None)

    def test_patma_199(self):
        class Color(int, enum.Enum):
            RED = 0
            GREEN = 1
            BLUE = 2
        def f(color):
            match color:
                case Color.RED:
                    return "I see red!"
                case Color.GREEN:
                    return "Grass is green"
                case Color.BLUE:
                    return "I'm feeling the blues :("
        self.assertEqual(f(Color.RED), "I see red!")
        self.assertEqual(f(Color.GREEN), "Grass is green")
        self.assertEqual(f(Color.BLUE), "I'm feeling the blues :(")
        self.assertIs(f(Color), None)
        self.assertEqual(f(0), "I see red!")
        self.assertEqual(f(1), "Grass is green")
        self.assertEqual(f(2), "I'm feeling the blues :(")
        self.assertIs(f(3), None)
        self.assertEqual(f(False), "I see red!")
        self.assertEqual(f(True), "Grass is green")
        self.assertEqual(f(2+0j), "I'm feeling the blues :(")
        self.assertIs(f(3.0), None)

    def test_patma_200(self):
        class Class:
            __match_args__ = ("a", "b")
        c = Class()
        c.a = 0
        c.b = 1
        match c:
            case Class(x, y):
                z = 0
        self.assertIs(x, c.a)
        self.assertIs(y, c.b)
        self.assertEqual(z, 0)

    def test_patma_201(self):
        class Class:
            __match_args__ = ("a", "b")
        c = Class()
        c.a = 0
        c.b = 1
        match c:
            case Class(x, b=y):
                z = 0
        self.assertIs(x, c.a)
        self.assertIs(y, c.b)
        self.assertEqual(z, 0)

    def test_patma_202(self):
        class Parent:
            __match_args__ = "a", "b"
        class Child(Parent):
            __match_args__ = ("c", "d")
        c = Child()
        c.a = 0
        c.b = 1
        match c:
            case Parent(x, y):
                z = 0
        self.assertIs(x, c.a)
        self.assertIs(y, c.b)
        self.assertEqual(z, 0)

    def test_patma_203(self):
        class Parent:
            __match_args__ = ("a", "b")
        class Child(Parent):
            __match_args__ = "c", "d"
        c = Child()
        c.a = 0
        c.b = 1
        match c:
            case Parent(x, b=y):
                z = 0
        self.assertIs(x, c.a)
        self.assertIs(y, c.b)
        self.assertEqual(z, 0)

    def test_patma_204(self):
        def f(w):
            match w:
                case 42:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f(42), {})
        self.assertIs(f(0), None)
        self.assertEqual(f(42.0), {})
        self.assertIs(f("42"), None)

    def test_patma_205(self):
        def f(w):
            match w:
                case 42.0:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f(42.0), {})
        self.assertEqual(f(42), {})
        self.assertIs(f(0.0), None)
        self.assertIs(f(0), None)

    def test_patma_206(self):
        def f(w):
            match w:
                case 1 | 2 | 3:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f(1), {})
        self.assertEqual(f(2), {})
        self.assertEqual(f(3), {})
        self.assertEqual(f(3.0), {})
        self.assertIs(f(0), None)
        self.assertIs(f(4), None)
        self.assertIs(f("1"), None)

    def test_patma_207(self):
        def f(w):
            match w:
                case [1, 2] | [3, 4]:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f([1, 2]), {})
        self.assertEqual(f([3, 4]), {})
        self.assertIs(f(42), None)
        self.assertIs(f([2, 3]), None)
        self.assertIs(f([1, 2, 3]), None)
        self.assertEqual(f([1, 2.0]), {})

    def test_patma_208(self):
        def f(w):
            match w:
                case x:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f(42), {"x": 42})
        self.assertEqual(f((1, 2)), {"x": (1, 2)})
        self.assertEqual(f(None), {"x": None})

    def test_patma_209(self):
        def f(w):
            match w:
                case _:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f(42), {})
        self.assertEqual(f(None), {})
        self.assertEqual(f((1, 2)), {})

    def test_patma_210(self):
        def f(w):
            match w:
                case (x, y, z):
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f((1, 2, 3)), {"x": 1, "y": 2, "z": 3})
        self.assertIs(f((1, 2)), None)
        self.assertIs(f((1, 2, 3, 4)), None)
        self.assertIs(f(123), None)
        self.assertIs(f("abc"), None)
        self.assertIs(f(b"abc"), None)
        self.assertEqual(f(array.array("b", b"abc")), {'x': 97, 'y': 98, 'z': 99})
        self.assertEqual(f(memoryview(b"abc")), {"x": 97, "y": 98, "z": 99})
        self.assertIs(f(bytearray(b"abc")), None)

    def test_patma_211(self):
        def f(w):
            match w:
                case {"x": x, "y": "y", "z": z}:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f({"x": "x", "y": "y", "z": "z"}), {"x": "x", "z": "z"})
        self.assertEqual(f({"x": "x", "y": "y", "z": "z", "a": "a"}), {"x": "x", "z": "z"})
        self.assertIs(f(({"x": "x", "y": "yy", "z": "z", "a": "a"})), None)
        self.assertIs(f(({"x": "x", "y": "y"})), None)

    def test_patma_212(self):
        def f(w):
            match w:
                case MyClass(int(xx), y="hello"):
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f(MyClass(42, "hello")), {"xx": 42})

    def test_patma_213(self):
        def f(w):
            match w:
                case (p, q) as x:
                    out = locals()
                    del out["w"]
                    return out
        self.assertEqual(f((1, 2)), {"p": 1, "q": 2, "x": (1, 2)})
        self.assertEqual(f([1, 2]), {"p": 1, "q": 2, "x": [1, 2]})
        self.assertIs(f(12), None)
        self.assertIs(f((1, 2, 3)), None)

    def test_patma_214(self):
        def f():
            match 42:
                case 42:
                    return locals()
        self.assertEqual(set(f()), set())

    def test_patma_215(self):
        def f():
            match 1:
                case 1 | 2 | 3:
                    return locals()
        self.assertEqual(set(f()), set())

    def test_patma_216(self):
        def f():
            match ...:
                case _:
                    return locals()
        self.assertEqual(set(f()), set())

    def test_patma_217(self):
        def f():
            match ...:
                case abc:
                    return locals()
        self.assertEqual(set(f()), {"abc"})

    @no_perf
    def test_patma_218(self):
        self.assert_syntax_error("""
        match ...:
            case "a" | a:
                pass
        """)

    @no_perf
    def test_patma_219(self):
        self.assert_syntax_error("""
        match ...:
            case a | "a":
                pass
        """)

    def test_patma_220(self):
        def f():
            match ..., ...:
                case a, b:
                    return locals()
        self.assertEqual(set(f()), {"a", "b"})

    @no_perf
    def test_patma_221(self):
        self.assert_syntax_error("""
        match ...:
            case a, a:
                pass
        """)

    def test_patma_222(self):
        def f():
            match {"k": ..., "l": ...}:
                case {"k": a, "l": b}:
                    return locals()
        self.assertEqual(set(f()), {"a", "b"})

    @no_perf
    def test_patma_223(self):
        self.assert_syntax_error("""
        match ...:
            case {"k": a, "l": a}:
                pass
        """)

    def test_patma_224(self):
        def f():
            match MyClass(..., ...):
                case MyClass(x, y=y):
                    return locals()
        self.assertEqual(set(f()), {"x", "y"})

    @no_perf
    def test_patma_225(self):
        self.assert_syntax_error("""
        match ...:
            case MyClass(x, x):
                pass
        """)

    @no_perf
    def test_patma_226(self):
        self.assert_syntax_error("""
        match ...:
            case MyClass(x=x, y=x):
                pass
        """)

    @no_perf
    def test_patma_227(self):
        self.assert_syntax_error("""
        match ...:
            case MyClass(x, y=x):
                pass
        """)

    def test_patma_228(self):
        def f():
            match ...:
                case b as a:
                    return locals()
        self.assertEqual(set(f()), {"a", "b"})

    @no_perf
    def test_patma_229(self):
        self.assert_syntax_error("""
        match ...:
            case a as a:
                pass
        """)

    def test_patma_230(self):
        def f(x):
            match x:
                case _:
                    return 0
        self.assertEqual(f(0), 0)
        self.assertEqual(f(1), 0)
        self.assertEqual(f(2), 0)
        self.assertEqual(f(3), 0)

    def test_patma_231(self):
        def f(x):
            match x:
                case 0:
                    return 0
        self.assertEqual(f(0), 0)
        self.assertIs(f(1), None)
        self.assertIs(f(2), None)
        self.assertIs(f(3), None)

    def test_patma_232(self):
        def f(x):
            match x:
                case 0:
                    return 0
                case _:
                    return 1
        self.assertEqual(f(0), 0)
        self.assertEqual(f(1), 1)
        self.assertEqual(f(2), 1)
        self.assertEqual(f(3), 1)

    def test_patma_233(self):
        def f(x):
            match x:
                case 0:
                    return 0
                case 1:
                    return 1
        self.assertEqual(f(0), 0)
        self.assertEqual(f(1), 1)
        self.assertIs(f(2), None)
        self.assertIs(f(3), None)

    def test_patma_234(self):
        def f(x):
            match x:
                case 0:
                    return 0
                case 1:
                    return 1
                case _:
                    return 2
        self.assertEqual(f(0), 0)
        self.assertEqual(f(1), 1)
        self.assertEqual(f(2), 2)
        self.assertEqual(f(3), 2)

    def test_patma_235(self):
        def f(x):
            match x:
                case 0:
                    return 0
                case 1:
                    return 1
                case 2:
                    return 2
        self.assertEqual(f(0), 0)
        self.assertEqual(f(1), 1)
        self.assertEqual(f(2), 2)
        self.assertIs(f(3), None)

    @no_perf
    def test_patma_236(self):
        self.assert_syntax_error("""
        match ...:
            case {**rest, "key": value}:
                pass
        """)

    @no_perf
    def test_patma_237(self):
        self.assert_syntax_error("""
        match ...:
            case {"first": first, **rest, "last": last}:
                pass
        """)

    @no_perf
    def test_patma_238(self):
        self.assert_syntax_error("""
        match ...:
            case *a, b, *c, d, *e:
                pass
        """)

    @no_perf
    def test_patma_239(self):
        self.assert_syntax_error("""
        match ...:
            case a, *b, c, *d, e:
                pass
        """)

    @no_perf
    def test_patma_240(self):
        self.assert_syntax_error("""
        match ...:
            case 0+0:
                pass
        """)

    @no_perf
    def test_patma_241(self):
        self.assert_syntax_error("""
        match ...:
            case f"":
                pass
        """)

    @no_perf
    def test_patma_242(self):
        self.assert_syntax_error("""
        match ...:
            case f"{x}":
                pass
        """)

    @no_perf
    def test_patma_243(self):
        self.assert_syntax_error("""
        match 42:
            case x:
                pass
            case y:
                pass
        """)

    @no_perf
    def test_patma_244(self):
        self.assert_syntax_error("""
        match ...:
            case {**_}:
                pass
        """)

    @no_perf
    def test_patma_245(self):
        self.assert_syntax_error("""
        match ...:
            case 42 as _:
                pass
        """)

    @no_perf
    def test_patma_246(self):
        class Class:
            __match_args__ = None
        x = Class()
        y = z = None
        with self.assertRaises(TypeError):
            match x:
                case Class(y):
                    z = 0
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_247(self):
        class Class:
            __match_args__ = "XYZ"
        x = Class()
        y = z = None
        with self.assertRaises(TypeError):
            match x:
                case Class(y):
                    z = 0
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_248(self):
        class Class:
            __match_args__ = (None,)
        x = Class()
        y = z = None
        with self.assertRaises(TypeError):
            match x:
                case Class(y):
                    z = 0
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_249(self):
        class Class:
            __match_args__ = ()
        x = Class()
        y = z = None
        with self.assertRaises(TypeError):
            match x:
                case Class(y):
                    z = 0
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_250(self):
        self.assert_syntax_error("""
        match ...:
            case Class(a=_, a=_):
                pass
        """)

    @no_perf
    def test_patma_251(self):
        x = {"a": 0, "b": 1}
        w = y = z = None
        with self.assertRaises(ValueError):
            match x:
                case {"a": y, "a": z}:
                    w = 0
        self.assertIs(w, None)
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_252(self):
        class Keys:
            KEY = "a"
        x = {"a": 0, "b": 1}
        w = y = z = None
        with self.assertRaises(ValueError):
            match x:
                case {Keys.KEY: y, "a": z}:
                    w = 0
        self.assertIs(w, None)
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_253(self):
        class Class:
            __match_args__ = ("a", "a")
            a = None
        x = Class()
        w = y = z = None
        with self.assertRaises(TypeError):
            match x:
                case Class(y, z):
                    w = 0
        self.assertIs(w, None)
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_254(self):
        class Class:
            __match_args__ = ("a",)
            a = None
        x = Class()
        w = y = z = None
        with self.assertRaises(TypeError):
            match x:
                case Class(y, a=z):
                    w = 0
        self.assertIs(w, None)
        self.assertIs(y, None)
        self.assertIs(z, None)

    def test_patma_255(self):
        match():
            case():
                x = 0
        self.assertEqual(x, 0)

    def test_patma_256(self):
        x = 0
        match(x):
            case(x):
                y = 0
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)

    def test_patma_257(self):
        x = 0
        match x:
            case False:
                y = 0
            case 0:
                y = 1
        self.assertEqual(x, 0)
        self.assertEqual(y, 1)

    def test_patma_258(self):
        x = 1
        match x:
            case True:
                y = 0
            case 1:
                y = 1
        self.assertEqual(x, 1)
        self.assertEqual(y, 1)

    def test_patma_259(self):
        class Eq:
            def __eq__(self, other):
                return True
        x = eq = Eq()
        y = None
        match x:
            case None:
                y = 0
        self.assertIs(x, eq)
        self.assertEqual(y, None)

    def test_patma_260(self):
        x = False
        match x:
            case False:
                y = 0
        self.assertIs(x, False)
        self.assertEqual(y, 0)

    def test_patma_261(self):
        x = True
        match x:
            case True:
                y = 0
        self.assertIs(x, True)
        self.assertEqual(y, 0)

    def test_patma_262(self):
        x = None
        match x:
            case None:
                y = 0
        self.assertIs(x, None)
        self.assertEqual(y, 0)

    def test_patma_263(self):
        x = 0
        match x:
            case (0 as w) as z:
                y = 0
        self.assertEqual(w, 0)
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertEqual(z, 0)

    def test_patma_264(self):
        x = 0
        match x:
            case (0 as w) as z:
                y = 0
        self.assertEqual(w, 0)
        self.assertEqual(x, 0)
        self.assertEqual(y, 0)
        self.assertEqual(z, 0)

    def test_patma_265(self):
        x = ((0, 1), (2, 3))
        match x:
            case ((a as b, c as d) as e) as w, ((f as g, h) as i) as z:
                y = 0
        self.assertEqual(a, 0)
        self.assertEqual(b, 0)
        self.assertEqual(c, 1)
        self.assertEqual(d, 1)
        self.assertEqual(e, (0, 1))
        self.assertEqual(f, 2)
        self.assertEqual(g, 2)
        self.assertEqual(h, 3)
        self.assertEqual(i, (2, 3))
        self.assertEqual(w, (0, 1))
        self.assertEqual(x, ((0, 1), (2, 3)))
        self.assertEqual(y, 0)
        self.assertEqual(z, (2, 3))

    @no_perf
    def test_patma_266(self):
        self.assert_syntax_error("""
        match ...:
            case _ | _:
                pass
        """)

    @no_perf
    def test_patma_267(self):
        self.assert_syntax_error("""
        match ...:
            case (_ as x) | [x]:
                pass
        """)


    @no_perf
    def test_patma_268(self):
        self.assert_syntax_error("""
        match ...:
            case _ | _ if condition():
                pass
        """)


    @no_perf
    def test_patma_269(self):
        self.assert_syntax_error("""
        match ...:
            case x | [_ as x] if x:
                pass
        """)

    @no_perf
    def test_patma_270(self):
        self.assert_syntax_error("""
        match ...:
            case _:
                pass
            case None:
                pass
        """)

    @no_perf
    def test_patma_271(self):
        self.assert_syntax_error("""
        match ...:
            case x:
                pass
            case [x] if x:
                pass
        """)

    @no_perf
    def test_patma_272(self):
        self.assert_syntax_error("""
        match ...:
            case x:
                pass
            case _:
                pass
        """)

    @no_perf
    def test_patma_273(self):
        self.assert_syntax_error("""
        match ...:
            case (None | _) | _:
                pass
        """)

    @no_perf
    def test_patma_274(self):
        self.assert_syntax_error("""
        match ...:
            case _ | (True | False):
                pass
        """)

    def test_patma_275(self):
        x = collections.UserDict({0: 1, 2: 3})
        match x:
            case {2: 3}:
                y = 0
        self.assertEqual(x, {0: 1, 2: 3})
        self.assertEqual(y, 0)

    def test_patma_276(self):
        x = collections.UserDict({0: 1, 2: 3})
        match x:
            case {2: 3, **z}:
                y = 0
        self.assertEqual(x, {0: 1, 2: 3})
        self.assertEqual(y, 0)
        self.assertEqual(z, {0: 1})

    def test_patma_277(self):
        x = [[{0: 0}]]
        match x:
            case list([({-0-0j: int(real=0+0j, imag=0-0j) | (1) as z},)]):
                y = 0
        self.assertEqual(x, [[{0: 0}]])
        self.assertEqual(y, 0)
        self.assertEqual(z, 0)

    def test_patma_278(self):
        x = range(3)
        match x:
            case [y, *_, z]:
                w = 0
        self.assertEqual(w, 0)
        self.assertEqual(x, range(3))
        self.assertEqual(y, 0)
        self.assertEqual(z, 2)

    def test_patma_279(self):
        x = range(3)
        match x:
            case [_, *_, y]:
                z = 0
        self.assertEqual(x, range(3))
        self.assertEqual(y, 2)
        self.assertEqual(z, 0)

    def test_patma_280(self):
        x = range(3)
        match x:
            case [*_, y]:
                z = 0
        self.assertEqual(x, range(3))
        self.assertEqual(y, 2)
        self.assertEqual(z, 0)

    @no_perf
    def test_patma_281(self):
        x = range(10)
        y = None
        with self.assertRaises(TypeError):
            match x:
                case range(10):
                    y = 0
        self.assertEqual(x, range(10))
        self.assertIs(y, None)

    @no_perf
    def test_patma_282(self):
        class Class:
            __match_args__ = ["spam", "eggs"]
            spam = 0
            eggs = 1
        x = Class()
        w = y = z = None
        with self.assertRaises(TypeError):
            match x:
                case Class(y, z):
                    w = 0
        self.assertIs(w, None)
        self.assertIs(y, None)
        self.assertIs(z, None)

    @no_perf
    def test_patma_283(self):
        self.assert_syntax_error("""
        match ...:
            case {0+0: _}:
                pass
        """)

    @no_perf
    def test_patma_284(self):
        self.assert_syntax_error("""
        match ...:
            case {f"": _}:
                pass
        """)

    @no_perf
    def test_patma_285(self):
        self.assert_syntax_error("""
        match ...:
            case 0j+0:
                pass
        """)

    @no_perf
    def test_patma_286(self):
        self.assert_syntax_error("""
        match ...:
            case 0j+0j:
                pass
        """)

    @no_perf
    def test_patma_287(self):
        self.assert_syntax_error("""
        match ...:
            case {0j+0: _}:
                pass
        """)

    @no_perf
    def test_patma_288(self):
        self.assert_syntax_error("""
        match ...:
            case {0j+0j: _}:
                pass
        """)

    def test_patma_289(self):
        x = {"y": 1}
        match x:
            case {"y": (0 as y) | (1 as y)}:
                z = 0
        self.assertEqual(x, {"y": 1})
        self.assertEqual(y, 1)
        self.assertEqual(z, 0)

    @no_perf
    def test_patma_290(self):
        self.assert_syntax_error("""
        match ...:
            case [a, [b] | [c] | [d]]:
                pass
        """)

    @no_perf
    def test_patma_291(self):
        # Hunting for leaks using -R doesn't catch leaks in the compiler itself,
        # just the code under test. This test ensures that if there are leaks in
        # the pattern compiler, those runs will fail:
        with open(__file__) as file:
            compile(file.read(), __file__, "exec")

    def test_patma_292(self):
        def f(x):
            match x:
                case ((a, b, c, d, e, f, g, h, i, 9) |
                      (h, g, i, a, b, d, e, c, f, 10) |
                      (g, b, a, c, d, -5, e, h, i, f) |
                      (-1, d, f, b, g, e, i, a, h, c)):
                    w = 0
            out = locals()
            del out["x"]
            return out
        alts = [
            dict(a=0, b=1, c=2, d=3, e=4, f=5, g=6, h=7, i=8, w=0),
            dict(h=1, g=2, i=3, a=4, b=5, d=6, e=7, c=8, f=9, w=0),
            dict(g=0, b=-1, a=-2, c=-3, d=-4, e=-6, h=-7, i=-8, f=-9, w=0),
            dict(d=-2, f=-3, b=-4, g=-5, e=-6, i=-7, a=-8, h=-9, c=-10, w=0),
            dict(),
        ]
        self.assertEqual(f(range(10)), alts[0])
        self.assertEqual(f(range(1, 11)), alts[1])
        self.assertEqual(f(range(0, -10, -1)), alts[2])
        self.assertEqual(f(range(-1, -11, -1)), alts[3])
        self.assertEqual(f(range(10, 20)), alts[4])

    def test_patma_293(self):
        def f(x):
            match x:
                case [y, (a, b, c, d, e, f, g, h, i, 9) |
                         (h, g, i, a, b, d, e, c, f, 10) |
                         (g, b, a, c, d, -5, e, h, i, f) |
                         (-1, d, f, b, g, e, i, a, h, c), z]:
                    w = 0
            out = locals()
            del out["x"]
            return out
        alts = [
            dict(a=0, b=1, c=2, d=3, e=4, f=5, g=6, h=7, i=8, w=0, y=False, z=True),
            dict(h=1, g=2, i=3, a=4, b=5, d=6, e=7, c=8, f=9, w=0, y=False, z=True),
            dict(g=0, b=-1, a=-2, c=-3, d=-4, e=-6, h=-7, i=-8, f=-9, w=0, y=False, z=True),
            dict(d=-2, f=-3, b=-4, g=-5, e=-6, i=-7, a=-8, h=-9, c=-10, w=0, y=False, z=True),
            dict(),
        ]
        self.assertEqual(f((False, range(10), True)), alts[0])
        self.assertEqual(f((False, range(1, 11), True)), alts[1])
        self.assertEqual(f((False, range(0, -10, -1), True)), alts[2])
        self.assertEqual(f((False, range(-1, -11, -1), True)), alts[3])
        self.assertEqual(f((False, range(10, 20), True)), alts[4])

    def test_patma_294(self):
        def f(x):
            y = None
            match x:
                case 0: y = (0,)
                case 1: y = (1,)
                case 2: y = (2,)
                case 3: y = (3,)
                case 4: y = (4,)
                case 5: y = (5,)
                case 6: y = (6,)
                case 7: y = (7,)
                case 8: y = (8,)
                case 9: y = (9,)
                case 10: y = (10,)
                case 11: y = (11,)
                case 12: y = (12,)
                case 13: y = (13,)
                case 14: y = (14,)
                case 15: y = (15,)
                case 16: y = (16,)
                case 17: y = (17,)
                case 18: y = (18,)
                case 19: y = (19,)
                case 20: y = (20,)
                case 21: y = (21,)
                case 22: y = (22,)
                case 23: y = (23,)
                case 24: y = (24,)
                case 25: y = (25,)
                case 26: y = (26,)
                case 27: y = (27,)
                case 28: y = (28,)
                case 29: y = (29,)
            return y
        result = {x: f(x) for x in range(30)}
        expected = {x: (x,) for x in range(30)}
        self.assertEqual(result, expected)


class TestInheritance(unittest.TestCase):

    def test_multiple_inheritance(self):
        class C:
            pass
        class S1(collections.UserList, collections.abc.Mapping):
            pass
        class S2(C, collections.UserList, collections.abc.Mapping):
            pass
        class S3(list, C, collections.abc.Mapping):
            pass
        class S4(collections.UserList, dict, C):
            pass
        class M1(collections.UserDict, collections.abc.Sequence):
            pass
        class M2(C, collections.UserDict, collections.abc.Sequence):
            pass
        class M3(collections.UserDict, C, list):
            pass
        class M4(dict, collections.abc.Sequence, C):
            pass
        def f(x):
            match x:
                case []:
                    return "seq"
                case {}:
                    return "map"
        def g(x):
            match x:
                case {}:
                    return "map"
                case []:
                    return "seq"
        for Seq in (S1, S2, S3, S4):
            self.assertEqual(f(Seq()), "seq")
            self.assertEqual(g(Seq()), "seq")
        for Map in (M1, M2, M3, M4):
            self.assertEqual(f(Map()), "map")
            self.assertEqual(g(Map()), "map")


class TestJumpTables(unittest.TestCase):

    def assert_has_jump_tables(self, n):
        JUMP_TABLE_TYPE = dict
        def decorator(func):
            number = sum(type(x) is JUMP_TABLE_TYPE
                         for x in func.__code__.co_consts)
            self.assertEqual(number, n)
            return func
        return decorator

    def test_no_table(self):
        """Don't add a jump table where it wouldn't make sense."""

        @self.assert_has_jump_tables(0)
        def f1(x):
            match x:
                case _:
                    do_stuff()

        @self.assert_has_jump_tables(0)
        def f2(x):
            match x:
                case _:
                    do_stuff()

        @self.assert_has_jump_tables(0)
        def f3(x):
            match x:
                case 1:
                    do_stuff()

        @self.assert_has_jump_tables(0)
        def f4(x):
            match x:
                case 1:
                    do_stuff()
                case x:
                    do_stuff()


        @self.assert_has_jump_tables(0)
        def f5(x):
            match x:
                case 1:
                    do_stuff()
                case (y, z):
                    do_stuff()
                case 2:
                    do_stuff()
                case int():
                    do_stuff()
                case "Python":
                    do_stuff()
                case str():
                    do_stuff()
                case None:
                    do_stuff()
                case _:
                    do_stuff()

    def test_basic_table_ints(self):
        @self.assert_has_jump_tables(1)
        def negate(x):
            match x:
                case 1:
                    return -1
                case 2:
                    return -2
                case 3:
                    return -3
                case 4:
                    return -4
                case 5:
                    return -5
            return -6
        results = {x: negate(x) for x in (1, 2, 3, 4, 5, 6)}
        expected = {x: -x for x in (1, 2, 3, 4, 5, 6)}
        self.assertEqual(expected, results)

    def test_basic_table_strings_and_None(self):
        @self.assert_has_jump_tables(1)
        def f(x):
            match x:
                case "1":
                    y = 1
                case "2":
                    y = 2
                case "3":
                    y = 3
                case None:
                    return None
            return y

        self.assertEqual(f("1"), 1)
        self.assertEqual(f("2"), 2)
        self.assertEqual(f("3"), 3)
        self.assertEqual(f(None), None)

    def test_deopt(self):
        """Fall back onto case-by-case search when jumping isn't safe."""

        @self.assert_has_jump_tables(1)
        def f1(x):
            match x:
                case 1:
                    return -1
                case 2:
                    return -2
                case 3:
                    return -3
                case _:
                    return "else"

        @self.assert_has_jump_tables(1)
        def f2(x):
            match x:
                case 1:
                    return -1
                case 2:
                    return -2
                case 3:
                    return -3
            return "else"

        for f in f1, f2:
            with self.subTest(f=f.__name__):

                self.assertEqual(f(1), -1)
                self.assertEqual(f(2), -2)
                self.assertEqual(f(3), -3)
                self.assertEqual(f([]), "else")
                self.assertEqual(f({}), "else")
                self.assertEqual(f(set()), "else")
                self.assertEqual(f(lambda: None), "else")

                class A:
                    pass
                self.assertEqual(f(A), "else")
                self.assertEqual(f(A()), "else")

                class UnhashableInt(int):
                    def __hash__(self):
                        self.fail("Shouldn't hash subclasses.")

                self.assertEqual(f(UnhashableInt(1)), -1)
                self.assertEqual(f(UnhashableInt(2)), -2)
                self.assertEqual(f(UnhashableInt(3)), -3)
                self.assertEqual(f(UnhashableInt(4)), "else")

                class Two():
                    def __eq__(self, other):
                        return 2 == other

                self.assertEqual(f(Two()), -2)

    def test_deopt2(self):
        """Can't use jump tables on subclasses"""

        class ComparisonRecorder(int):
            def __new__(cls, val):
                obj = super().__new__(cls, val)
                obj.compares = []
                return obj
            def __eq__(self, other):
                self.compares.append(other)
                return super().__eq__(other)

        @self.assert_has_jump_tables(1)
        def get_compares1(x):
            c = ComparisonRecorder(x)
            match c:
                case 1: pass
                case 2: pass
                case 3: pass
                case 4: pass
                case 5: pass
            return c.compares

        @self.assert_has_jump_tables(1)
        def get_compares2(x):
            c = ComparisonRecorder(x)
            match c:
                case []: pass
                case 1: pass
                case 2: pass
                case 3: pass
                case 4: pass
                case 5: pass
                case _: pass
            return c.compares

        for get_compares in get_compares1, get_compares2:
            with self.subTest(get_compares.__name__):
                self.assertEqual(get_compares(1), [1])
                self.assertEqual(get_compares(2), [1, 2])
                self.assertEqual(get_compares(3), [1, 2, 3])
                self.assertEqual(get_compares(4), [1, 2, 3, 4])
                self.assertEqual(get_compares(5), [1, 2, 3, 4, 5])
                self.assertEqual(get_compares(6), [1, 2, 3, 4, 5])


    def test_basic_table_with_catchall(self):
        @self.assert_has_jump_tables(1)
        def negate1(x):
            match x:
                case 1:
                    return -1
                case 2:
                    return -2
                case 3:
                    return -3
                case z:
                    return -z

        @self.assert_has_jump_tables(1)
        def negate2(x):
            match x:
                case 1:
                    return -1
                case 2:
                    return -2
                case 3:
                    return -3
                case int(z):
                    return -z

        @self.assert_has_jump_tables(1)
        def negate3(x):
            match x:
                case 1:
                    return -1
                case 2:
                    return -2
                case 3:
                    return -3
                case _:
                    pass
            return -x

        @self.assert_has_jump_tables(1)
        def negate4(x):
            match x:
                case 1:
                    return -1
                case 2:
                    return -2
                case 3:
                    return -3
                case int() as z:
                    return -z
                case _:
                    raise TypeError()

        expected = {x: -x for x in (1, 2, 3, 4)}
        self.assertEqual({x: negate1(x) for x in (1, 2, 3, 4)}, expected)
        self.assertEqual({x: negate2(x) for x in (1, 2, 3, 4)}, expected)
        self.assertEqual({x: negate3(x) for x in (1, 2, 3, 4)}, expected)
        self.assertEqual({x: negate4(x) for x in (1, 2, 3, 4)}, expected)


    def test_multiple_jump_tables_per_function(self):
        @self.assert_has_jump_tables(3)
        def f(x):
            match x:
                case "1": y = 1
                case "2": y = 2
                case "3": y = 3
            match x:
                case "4": y = 4
                case "5": y = 5
                case "6": y = 6
            match x:
                case "7": y = 7
                case "8": y = 8
                case "9": y = 9
            return y

        expected = {str(x): x for x in range(1, 10)}
        self.assertEqual({x: f(x) for x in expected}, expected)

    def test_nested_jump_tables(self):
        @self.assert_has_jump_tables(4)
        def add(x, y):
            match x:
                case 1:
                    match y:
                        case 1: return 2
                        case 2: return 3
                        case 3: return 4
                case 2:
                    match y:
                        case 1: return 3
                        case 2: return 4
                        case 3: return 5
                case 3:
                    match y:
                        case 1: return 4
                        case 2: return 5
                        case 3: return 6
        expected = {(x, y): x + y for x in (1, 2, 3) for y in (1, 2, 3)}
        actual = {(x, y): add(x, y) for x in (1, 2, 3) for y in (1, 2, 3)}
        self.assertEqual(actual, expected)

    def test_separated_jump_table(self):
        @self.assert_has_jump_tables(3)
        def f(x):
            match x:
                case [a]: return a
                case -1: return 1
                case -2: return 2
                case -3: return 3
                case [a, b]: return a + b
                case -4: return 4
                case -5: return 5
                case -6: return 6
                case [a, b, c]: return a + b + c
                case -7: return 7
                case -8: return 8
                case -9: return 9
                case [a, b, c, d]: return a + b + c + d
        expected = ({-x: x for x in range(1, 10)}
                    | {(1,)*n: n for n in range(1, 5)})
        actual = {x: f(x) for x in expected}
        self.assertEqual(actual, expected)

    def test_separated_jump_table2(self):
        @self.assert_has_jump_tables(2)
        def f(x):
            match x:
                case "a": return "A"
                case "b": return "B"
                case "c": return "C"
                case (): return "tuple"
                case "A": return "a"
                case "B": return "b"
                case "C": return "c"
        expected = dict(zip("abcABC", "ABCabc"))
        actual = {x: f(x) for x in expected}
        self.assertEqual(expected, actual)

    def test_or_patterns1(self):
        @self.assert_has_jump_tables(1)
        def f(x):
            match x:
                case 1 | 2 | 3 | 4 | "apple":
                    y = True
                case 5 | 6 | 7 | 8 | "banana":
                    y = False
                case _:
                    raise ValueError()
            return y
        expected = {1: True, 2: True, 3: True, 4: True, "apple": True,
                    5: False, 6: False, 7: False, 8: False, "banana": False}
        actual = {x: f(x) for x in expected}
        self.assertEqual(expected, actual)

    def test_or_patterns2(self):
        @self.assert_has_jump_tables(1)
        def f(x):
            y = False
            match x:
                case 1 | 2 | 3 | 4 | "apple":
                    y = True
            return y
        expected = {1: True, 2: True, 3: True, 4: True, "apple": True,
                    5: False, 6: False, 7: False, 8: False, "banana": False}
        actual = {x: f(x) for x in expected}
        self.assertEqual(expected, actual)

    def test_or_patterns3(self):
        @self.assert_has_jump_tables(2)
        def f(x):
            match x:
                case (a, b):
                    return "a"
                case 1 | 2 | 3:
                    return "b"
                case (a, b, c):
                    return "c"
                case None:
                    return "d"
                case 4 | 5 | 6:
                    return "e"
                case x:
                    return "f"
        expected = {(0, 0): "a",
                    1: "b", 2: "b", 3: "b",
                    (0, 0, 0):
                    "c", None:
                    "d", 4: "e", 5: "e", 6: "e",
                    17: "f"}
        actual = {x: f(x) for x in expected}
        self.assertEqual(expected, actual)


class PerfPatma(TestPatma):

    def assertEqual(*_, **__):
        pass

    def assertIs(*_, **__):
        pass

    def assertRaises(*_, **__):
        assert False, "this test should be decorated with @no_perf!"

    def assertWarns(*_, **__):
        assert False, "this test should be decorated with @no_perf!"

    def run_perf(self):
        attrs = vars(TestPatma).items()
        tests = [
            attr for name, attr in attrs
            if name.startswith("test_") and not hasattr(attr, "no_perf")
        ]
        for _ in range(1 << 8):
            for test in tests:
                test(self)

    @staticmethod
    def setUpClass():
        raise unittest.SkipTest("performance testing")


"""
# From inside venv pointing to this Python, with pyperf installed:
sudo $(which python) -m pyperf system tune && \
     $(which python) -m pyperf timeit --rigorous --setup "from test.test_patma import PerfPatma; p = PerfPatma()" "p.run_perf()"; \
sudo $(which python) -m pyperf system reset
"""
