# SOURCE:MVM

**Source Movie Maker** — a modern, in-game cinematics and demo-replay toolkit for Counter-Strike.

SOURCE:MVM is a fork of [HLAE / advancedfx](https://github.com/advancedfx/advancedfx) focused on making CS2 movie-making feel native. The goal is a clean, Panorama-integrated workflow for browsing demos, building camera paths, and editing shots without leaving the game.

---

## ⚠️ Warnings

> **VAC warning** — This tool is technically a hack. Only use it for making videos or watching demos. Joining VAC-protected servers with it will probably get you VAC banned.

> **Epilepsy warning** — This software can cause fast-changing images and colors on your screen.

---

## Game support

| Engine              | Game(s)                                           | Status                          |
| ------------------- | ------------------------------------------------- | ------------------------------- |
| **Source 2**        | Counter-Strike 2                                  | ✅ **In active development**    |
| Source / GoldSource | CS:GO, CS 1.6, and older Source/GoldSource titles | ⏸️ Not currently in development |

Right now all effort is on **CS2 (Source 2)**. The older GoldSource / Source code inherited from upstream is left intact but isn't being worked on here.

---

## What's here

- **Panorama "Demos" tab** — browse and launch your demos straight from the CS2 main menu, with parsed scoreboards and match info.
- **Camera editor mode** — a toggleable in-game editor workspace: scaled preview viewport, inspector, hosted timeline, and HUD hiding.
- **Camera-path / dolly system** — BO2-style markers and smooth dolly paths with hotkeys and a Panorama menu.
- **Director HUD & free-cam** — free-cam while paused and a native Panorama director HUD with camera-director controls.
- **Movie export pipeline** — the full HLAE recording/export toolchain under the hood.

## Coming soon

- A more complete timeline editor for keyframing and easing camera moves.
- Tighter Panorama integration across the editor and director tools.
- Quality-of-life passes on the demo browser and export workflow.

_(Features are evolving — expect rough edges.)_

---

## System requirements

- Windows 10 or newer
- .NET Framework 4.6.2 or newer
- A genuine Steam copy of CS2
- A lot of patience

---

## How to build

See [BUILDING.md](BUILDING.md).

## License

This repository contains the parts of the advancedfx project that are under the MIT license. See [LICENSE](LICENSE). **Note: the license does not apply to submodules.**

Credits: [CREDITS.md](CREDITS.md)

---

## Looking for the original HLAE?

This is a fork. For the full upstream documentation, releases, manual, and Discord support, see the original project:

**[advancedfx/advancedfx →](https://github.com/advancedfx/advancedfx)**

The previous upstream README is preserved there.

```


                                                             """"""""""
                                                           """"""""""""""
                                                          """"        """"
                                                         """           """
                                                         ""             ""
                                                         ""            """                          "
                                                    """""""           """" """                     """
                                                  """""""""           """"""""""""""""""""""""""""""""
                                                 """"                 """""" """""""""""""""""""""""""""""""""""
                                                """                                        """"""""  """"""""""""
                                               """                               ""     """""""""""""""""""""""""
                                               ""                              """"    """""""""""""""""""""""""
                                              """                             """""   """
                                              """                             """"    """
                                              """                              ""     ""
                                            """"                                     """
                                           """""                                     ""
                                          """"                        """"          """
                                          ""                          """"""""    """"
                                          ""                          """""""""""""""
                                          ""                           """  """""""
          """"""""""""""""""""""          ""                           """                 """"""""""""""""""""""""
        """""""""""""""""""""""I""        """                          """               """""""""""""""""""""""""I""
       """"                     ""         ""                          ""               """"                      """"
       ""   """""""""""""""""""""""        ""                          ""              """   """""""""""""""""""""""""
       ""   """"""""""""""""""""""         ""                         """              """   """"""""""""""""""""""""
       ""   ""                             ""                      """"""              """   ""
       ""   ""                             ""                      """""               """   """""""""""""""""""""""
       ""   ""                             ""                      """                  """   """"""""""""""""""""""""
       ""   ""                            """                       """"                """"""""""""""""""""""""   """
       ""   ""                            ""                         """"                 """""""""""""""""""""""   ""
       ""   ""                            ""                          """"                    "  "  "  "  "  """"   """
       ""   """"""""""""""""""""""        ""                            """              """"""""""""""""""""""""   ""
       ""   """"""""""""""""""""""        """           ""               ""             """""""""""""""""""""""""   ""
       """                      ""        ""          """"""""           """            """                        """
        """""""""""""""""""""""I""        ""         """"""""""           """            ""I""""""""""""""""""""""I""
         """""""""""""""""""""""         """         """    """            """             """"""""""""""""""""""""
                                         """        """      """"           """
                                         """       """        """""""        ""
                                         """       """         """"""        ""
                                        """       """              """       ""
                                       """"     """"               ""        ""
                                      """      """"                ""       """
                                     """       ""                  ""       """
                                     ""       """                  ""       """
                                    """       ""                   ""       """
                                    ""       """                   """      """
                                   """      """                     ""      ""
                                   ""       """                     """     ""
                                   ""     """"                      """     ""
                                   ""    """"                       """    """
                                  """    ""                         ""     """
                                  ""    """                        """      """"
                                 """    ""                         ""        """"""
                                 ""     ""                         """""""     """""
                                 ""     """                        """""""""""""""""
                                 """"""""""                              """""""""""
                                 """""""""
                                     "
```
