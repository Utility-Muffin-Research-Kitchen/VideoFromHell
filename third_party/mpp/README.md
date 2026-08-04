# Rockchip MPP ABI

`vfh_mpp_abi.h` is the deliberately small public Rockchip MPP ABI surface used
by the Phase 5 probe and direct H.264/HEVC decoder. It is derived from
Rockchip's Apache-2.0 public headers at commit `6dadc7e1` (2021-09-02). It is
limited to the verified MLP1 decoder path, not a claim of full compatibility
with every vendor MPP build.

The probe links against a generated `librockchip_mpp.so.1` SONAME stub.  The
stub is link-time metadata only: it is not put in a Pak and no Rockchip binary
is vendored. On the device, the binary resolves to `/usr/lib/librockchip_mpp.so.1`.

The MLP1 vendor runtime was exercised with MP4 demux/Annex-B conversion,
H.264/HEVC MPP decode, CPU copies of NV12 frames, and the real-player smoke
runner. Retain those device tests whenever this ABI surface changes.
