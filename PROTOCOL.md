# openCam wire protocol

Phone = server, PC = client. One PC at a time; a new connection replaces the old one.

- TCP `47100` on the phone. Wired: PC runs `adb forward tcp:47100 tcp:47100` and connects to `127.0.0.1:47100`.
- UDP `47101` on both sides, ASCII, sent from and answered to port 47101:
  - `OPENCAM1|<model>|<tcpPort>` phone→PC: beacon. Broadcast every 1 s while no PC is connected, and sent straight
    to a PC the user picks on the phone. A PC connects to any phone it hears from.
  - `OPENCAM?` PC→phone: scan, sent to every address of the PC's subnets (and to one address for a pairing code).
    The phone answers with its beacon. This gets through Windows Firewall even without an inbound rule.
  - `OPENCAMPC?` phone→PC: search; the PC answers `OPENCAMPC|<computer name>`.
- Pairing code: the phone's IPv4 address & 0xFFFF as 5 digits. The PC tries that host part on each of its own
  subnets' /16.

## Framing (TCP, both directions)

```
u8  type
u32 length (big-endian)
u8  payload[length]
```

| type | dir | payload |
|------|-----|---------|
| 1 CONFIG | phone→pc | H.264 SPS+PPS, Annex B |
| 2 FRAME  | phone→pc | `u8 keyframe` + `i64 ptsUs (BE)` + H.264 access unit, Annex B |
| 3 JSON   | both     | UTF-8 JSON object, field `t` = message kind |

## JSON messages

phone→pc
- `{"t":"caps", ...}`: what the current camera supports (zoom range, ev range, iso/exp range, focusMax, apertures, modes `["1920x1080@60",..]`, wb, torch, stab, manual, portrait, facings).
- `{"t":"state", ...}`: every setting plus `conn` (`usb`/`wifi`), `demand`, `cam` (camera open). Sent on every change; the phone is the source of truth.

pc→phone
- `{"t":"set","k":"<key>","v":<value>}`: change one setting. Keys match `state`.
- `{"t":"demand","v":true|false}`: PC needs frames (a Windows app opened the camera, or the PC window is visible). The phone keeps the camera closed when nobody needs it.
- `{"t":"key"}`: request a keyframe.

## State keys

`facing` back|front · `zoom` float · `mode` video|pro|portrait · `af` bool · `focus` diopters · `ev` int ·
`iso` int · `exp` ns · `wb` auto|daylight|cloudy|incandescent|fluorescent · `aperture` float ·
`torch` bool · `stab` bool · `res` "WxH" · `fps` int · `mbps` int (0 = auto) · `live` bool ·
`mirror` bool · `rot180` bool (the last two are applied on the PC side).
