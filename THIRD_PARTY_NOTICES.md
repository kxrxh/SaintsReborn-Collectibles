# Third-party notices

- [Saints Reborn](https://github.com/whompay/SaintsReborn), by whompay, supplies Whompay's Mod Loader and its native API. Native guest calls use a copied CPU context and a preserved scratch stack, following the integration pattern in the bundled trainer. The UI atlas uses Xbox 360 tiled addressing as implemented by the project's glyphgen/mapconv tools. The project is distributed under the Apache License 2.0; WML's API headers are marked MIT.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) supplies the CPU context headers used when compiling the mod. Those headers credit Tom Clay and the XenonRecomp/UnleashedRecomp infrastructure under the BSD 3-Clause License.
- [zlib](https://zlib.net/) by Jean-loup Gailly and Mark Adler is linked to read compressed files from the player's installed packfiles, under the zlib license.

The applicable license texts are in `third-party-licenses/`. WML, the SDK and Saints Row assets are not bundled in the release. Install Saints Reborn separately from its official project.
