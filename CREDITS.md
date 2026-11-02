# Credits

OpenSection computes every player in real time. It contains **no recordings**. The shipped model
(`models/opensection.osm`, embedded into the plugin) holds statistics measured from the sources
below: tone-colour curves per key (dB over a 1/6-octave grid), modulation depths, room width and
decay, and the colour difference between dynamic levels.

| what was learned | source | author | license |
|---|---|---|---|
| tone colour per key, bow unrest, room, release | Virtual Playing Orchestra 3 (all-strings section sustain) | Paul Battersby, <http://virtualplaying.com> – built on Sonatina Symphonic Orchestra, No Budget Orchestra, Mattias Westlund and VSCO2-CE samples | free (see the VPO licence) |
| soft vs. loud colour of a section, rosin-noise darkening | VSCO2 Community Edition (string section sustains) | Versilian Studios, <https://vis.versilstudios.com/vsco-community.html> | CC0 1.0 |
| pianissimo and fortissimo vs. forte colour | Philharmonia Orchestra sound samples (violin, viola, cello, double bass, arco) | Philharmonia Orchestra, London, <https://philharmonia.co.uk/resources/sound-samples/> | CC BY-SA 3.0 |
| pp / mf / ff colour (cross-check of the solo dynamics) | Musical Instrument Samples (violin, viola, cello, double bass, arco, anechoic, 2012 recordings) | University of Iowa Electronic Music Studios, Lawrence Fritts, <https://theremin.music.uiowa.edu/MIS.html> | "may be downloaded and used for any projects, without restrictions" |

The Philharmonia samples may not be redistributed in their original form; OpenSection does not
contain them. Insofar as the derived curves are an adaptation, they are shared under the same terms
as the rest of the project (GPL-3.0-or-later; CC BY-SA 3.0 → 4.0 → GPLv3 compatibility).

Thanks to all of them for making their work freely available.
