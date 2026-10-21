#!/usr/bin/env python3
"""Build the m17web user manual PDF.

    python3 docs/user_manual.py [out.pdf]

The text lives in this file so the manual can be edited and regenerated without
any other tooling.  Layout is plain reportlab platypus; table cells are
Paragraphs so long text wraps inside fixed column widths.
"""
from __future__ import annotations

import sys
from datetime import date

from reportlab.graphics.shapes import Circle, Drawing, Line, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm
from reportlab.platypus import (
    BaseDocTemplate,
    HRFlowable,
    Frame,
    ListFlowable,
    ListItem,
    PageBreak,
    PageTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)

TITLE = "m17web User Manual"
SUBTITLE = "M17 digital voice on a Raspberry Pi with an SDR, driven from your web browser"
VERSION = "Version 1.2 \u2014 " + date.today().strftime("%d %B %Y")

MARGIN = 2.7 * cm                 # ~87 characters per line: comfortable for body text
FRAME_W = A4[0] - 2 * MARGIN      # usable width between the margins

_ss = getSampleStyleSheet()
S = {
    "title": ParagraphStyle("t", parent=_ss["Title"], fontSize=26, leading=30, spaceAfter=6),
    "subtitle": ParagraphStyle("st", parent=_ss["BodyText"], fontSize=12.5, leading=17,
                               textColor=colors.HexColor("#444444"), alignment=TA_CENTER,
                               spaceAfter=4),
    "version": ParagraphStyle("v", parent=_ss["BodyText"], fontSize=10, leading=14,
                              textColor=colors.HexColor("#666666"), alignment=TA_CENTER),
    "h1": ParagraphStyle("h1", parent=_ss["Heading1"], fontSize=16, leading=20,
                         spaceBefore=16, spaceAfter=7, textColor=colors.HexColor("#12395c")),
    "h2": ParagraphStyle("h2", parent=_ss["Heading2"], fontSize=12.5, leading=16,
                         spaceBefore=11, spaceAfter=5, textColor=colors.HexColor("#2a5c86")),
    "p": ParagraphStyle("p", parent=_ss["BodyText"], fontSize=10.5, leading=15, spaceAfter=7),
    "li": ParagraphStyle("li", parent=_ss["BodyText"], fontSize=10.5, leading=15, spaceAfter=3),
    "note": ParagraphStyle("n", parent=_ss["BodyText"], fontSize=9.5, leading=13.5,
                           textColor=colors.HexColor("#555555"), spaceAfter=8),
    "cell": ParagraphStyle("c", parent=_ss["BodyText"], fontSize=9.5, leading=12.5, spaceAfter=0),
    "cellh": ParagraphStyle("ch", parent=_ss["BodyText"], fontSize=9.5, leading=12.5,
                            spaceAfter=0, fontName="Helvetica-Bold"),
    "code": ParagraphStyle("code", parent=_ss["BodyText"], fontSize=9.5, leading=13,
                           fontName="Courier", leftIndent=10, spaceAfter=6),
    "caption": ParagraphStyle("cap", parent=_ss["BodyText"], fontSize=9, leading=12.5,
                              textColor=colors.HexColor("#555555"), spaceBefore=4, spaceAfter=10),
}


class Manual:
    def __init__(self) -> None:
        self.story: list = []

    # -- building blocks ----------------------------------------------------
    def h1(self, text: str) -> None:
        self.story.append(Paragraph(text, S["h1"]))

    def h2(self, text: str) -> None:
        self.story.append(Paragraph(text, S["h2"]))

    def p(self, text: str) -> None:
        self.story.append(Paragraph(text, S["p"]))

    def note(self, text: str) -> None:
        self.story.append(Paragraph(text, S["note"]))

    def code(self, text: str) -> None:
        self.story.append(Paragraph(text, S["code"]))

    def bullets(self, items: list[str]) -> None:
        self.story.append(ListFlowable(
            [ListItem(Paragraph(i, S["li"]), leftIndent=14, value="bullet") for i in items],
            bulletType="bullet", start="\u2022", leftIndent=14, bulletFontSize=9,
            spaceBefore=2, spaceAfter=8,
        ))

    def steps(self, items: list[str]) -> None:
        self.story.append(ListFlowable(
            [ListItem(Paragraph(i, S["li"]), leftIndent=18) for i in items],
            bulletType="1", leftIndent=18, bulletFontSize=9.5,
            spaceBefore=2, spaceAfter=8,
        ))

    def table(self, rows: list[list[str]], widths: list[float], header: bool = True) -> None:
        data = []
        for r_i, row in enumerate(rows):
            style = S["cellh"] if (header and r_i == 0) else S["cell"]
            data.append([Paragraph(str(c), style) for c in row])
        total = sum(widths)
        cols = [w / total * FRAME_W for w in widths]
        t = Table(data, colWidths=cols, repeatRows=1 if header else 0)
        style = [
            ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#9aa5ae")),
            ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ("LEFTPADDING", (0, 0), (-1, -1), 5),
            ("RIGHTPADDING", (0, 0), (-1, -1), 5),
            ("TOPPADDING", (0, 0), (-1, -1), 4),
            ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
        ]
        if header:
            style.append(("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#e8eef4")))
        t.setStyle(TableStyle(style))
        self.story.append(t)
        self.story.append(Spacer(1, 10))

    def pagebreak(self) -> None:
        self.story.append(PageBreak())

    # -- the layout sketch --------------------------------------------------
    def sketch(self) -> None:
        """A labelled sketch of the panel layout - not a screenshot."""
        W, H = FRAME_W, 214
        d = Drawing(W, H)
        dark, light, edge = colors.HexColor("#12395c"), colors.HexColor("#eef3f7"), colors.HexColor("#8fa3b5")
        muted = colors.HexColor("#44586b")
        red = colors.HexColor("#c62828")
        d.add(Rect(0, H - 24, W, 24, fillColor=dark, strokeColor=dark))
        d.add(String(8, H - 17, "m17web", fontName="Helvetica-Bold", fontSize=11, fillColor=colors.white))
        for i, label in enumerate(["radio", "mode", "audio", "clients", "locator", "position", "gnss", "online"]):
            d.add(String(72 + i * 50, H - 17, label, fontName="Helvetica", fontSize=7,
                         fillColor=colors.HexColor("#cfe0ee")))

        def card(x, y, w, h, title, lines=()):
            d.add(Rect(x, y, w, h, fillColor=light, strokeColor=edge))
            d.add(String(x + 6, y + h - 12, title, fontName="Helvetica-Bold", fontSize=8.5, fillColor=dark))
            for i, t in enumerate(lines):
                d.add(String(x + 6, y + h - 24 - i * 10, t, fontName="Helvetica", fontSize=7,
                             fillColor=muted))

        pad, top = 6, H - 32
        lw = (W - 3 * pad) * 0.64              # the card area on the left
        cw = (lw - pad) / 2.0                  # two cards across it
        rx = pad + lw + pad                    # the console column
        rw = W - rx - pad
        col = lambda c: pad + c * (cw + pad)

        card(col(0), top - 52, cw, 46, "Station", [
            "my callsign      destination", "gateway      listen to all", "broadcast (ba)  help  clear log"])
        card(col(1), top - 52, cw, 46, "Radio", [
            "RX frequency (Hz)    TX frequency (Hz)", "RX gain [====]    TX gain [====]",
            "freq correction   apply   AFC", "transmit carrier    receive"])
        card(col(0), top - 110, cw, 52, "Reflector", [
            "reflector    module", "connect / disconnect"])
        card(col(1), top - 110, cw, 52, "Position", [
            "IO90ST          locator", "50.8036, -0.4419", "8 satellites   HDOP 1.2", "[ ] log to CSV  copy locator"])
        card(col(0), top - 168, cw, 52, "Audio", [
            "audio device    volume", "blank frames before voice", "mic / spk   transmit health"])
        card(col(1), top - 168, cw, 52, "PUSH TO TALK", [
            "signal \u25af\u25af\u25af\u25af\u25af   power \u25af\u25af\u25af",
            "RX / TX light     in frame", "break     carrier     duplex",
            "hardware PTT: GPIO 11 ready"])
        card(rx, top - 168, rw, 162, "Console / Command help", [
            "log lines ...", "", ">> type a command    [send]", "",
            "gps / csv / rf / tf / rg", "conn / disc / mode / audio"])

        # the compass, inside the Position card
        px, py = col(1) + cw - 22, top - 110 + 24
        d.add(Circle(px, py, 12, fillColor=colors.white, strokeColor=edge))
        d.add(String(px - 2.5, py + 13.5, "N", fontName="Helvetica", fontSize=5.5, fillColor=muted))
        d.add(Line(px, py, px + 7, py + 8, strokeColor=red, strokeWidth=1.5))
        d.add(String(px - 3.5, py - 8, "43", fontName="Helvetica", fontSize=5.5, fillColor=red))
        self.story.append(d)
        self.story.append(Paragraph(
            "Figure 1: the panels on the page. The header is always visible and carries your locator and "
            "position. The cards on the left are laid out two across - Station and Radio, Reflector and "
            "Position, then Audio with PUSH TO TALK at the bottom right - and the console and command help "
            "are the column on the right. The cards are compact enough that the whole page fits a "
            "screen without scrolling; the signal and power bars sit at the top of the PUSH TO TALK "
            "card. (Sketch, not a screenshot.)",
            S["caption"]))


def build(path: str) -> None:
    m = Manual()
    ed = m.story.append

    # ---------------------------------------------------------------- cover
    ed(Spacer(1, 4.2 * cm))
    m.h1("")   # ensure the title style block sits after the spacer
    m.story.pop()
    ed(Paragraph(TITLE, S["title"]))
    ed(Paragraph(SUBTITLE, S["subtitle"]))
    ed(Spacer(1, 0.6 * cm))
    ed(Paragraph(VERSION, S["version"]))
    ed(Spacer(1, 1.6 * cm))
    m.p("<b>This manual is for anyone using the radio.</b> It explains what the program does, "
        "how to start it, how to make a contact, and what to do when something does not work. "
        "You do not need to understand M17, SDR or Linux to use it \u2014 if someone set the system up "
        "for you, start at section 4.")
    m.p("A separate README in the same folder covers the technical details: building the program, "
        "the audio back ends, and the transmit timing.")
    m.h2("What is in this manual")
    m.table([
        ["", "Setting it up", "", "Using it"],
        ["1", "What m17web does", "7", "Audio: where your voice comes from and goes"],
        ["2", "What you need", "8", "The controls, panel by panel"],
        ["3", "Starting and stopping it", "9", "Commands"],
        ["4", "Opening the interface", "10", "If something goes wrong"],
        ["5", "The screen at a glance, your position and the locator", "11", "Good practice"],
        ["6", "Making a contact, and the PTT switch", "12", "Quick reference"],
    ], [0.4, 2.9, 0.4, 4.3])
    m.pagebreak()

    # ----------------------------------------------------------- 1. what it is
    m.h1("1. What m17web does")
    m.p("M17 is a digital voice mode for amateur radio: like FM voice, but it sends your speech as "
        "data, which gives clear audio and carries your callsign with it. m17web is a program that "
        "runs on a small computer (a Raspberry Pi) connected to a software-defined radio. It sends "
        "and receives M17 voice, and you control it entirely from a web page \u2014 no terminal "
        "commands needed in normal use.")
    m.p("Your voice comes from a microphone plugged into the Pi, or from the computer you are "
        "browsing from. Received voice comes out of the Pi's speaker (or your computer's).")
    m.note("If you are only practising, the program can run without a radio, in a mode that feeds "
           "your own audio straight back. See section 9.")

    # ------------------------------------------------------------ 2. what you need
    m.h1("2. What you need")
    m.table([
        ["Item", "Notes"],
        ["Raspberry Pi 4", "Any 4 GB or 8 GB model, with a network connection. The Pi runs m17web."],
        ["ADALM-Pluto (PlutoSDR / Pluto+)", "The radio itself, connected to the Pi by USB. A LimeSDR also works."],
        ["USB sound card with microphone and speaker or headphones",
         "For audio at the Pi. Recommended - it is the most reliable audio path."],
        ["Antenna and a valid amateur licence", "You need a licence for any frequency you transmit on."],
        ["A web browser", "On any computer, tablet or phone on the same network as the Pi."],
        ["<i>Optional:</i> a GPS receiver", "A small GPS receiver plugged into the Pi shows your position and "
         "Maidenhead locator on screen, and sends it to other stations (section 5)."],
        ["<i>Optional:</i> a PTT switch", "A switch wired to the Pi's header lets you transmit without the "
         "browser, from a hand microphone or a foot switch (section 6)."],
    ], [2.6, 5.4])

    # --------------------------------------------------------- 3. start and stop
    m.h1("3. Starting and stopping it")
    m.p("m17web is started from a terminal on the Pi. If someone else set the system up for you, it "
        "may already be running \u2014 in that case go straight to section 4.")
    m.p("<b>Start it</b> (this also works after a reboot):")
    m.code("cd /mnt/hf/shared/projects/m17_sdr_web/m17gismo<br/>"
           "RADIO=pluto MODE=radio bash tools/pi_m17web.sh start -a alsa")
    m.p("It keeps running after you close the terminal window. <b>Check it:</b>")
    m.code("bash tools/pi_m17web.sh status")
    m.p("<b>Stop it</b>, or look at the log to see what it is doing:")
    m.code("bash tools/pi_m17web.sh stop<br/>bash tools/pi_m17web.sh log")
    m.h2("What the settings mean")
    m.table([
        ["Setting", "What it does"],
        ["RADIO=pluto", "Use the PlutoSDR. Use <b>RADIO=sim</b> instead to practise with no radio attached."],
        ["MODE=radio", "Radio in, local sound out \u2014 the normal voice mode. It can also be changed "
                       "while the program is running, from the Mode panel (section 8)."],
        ["-a alsa", "Send the voice to the Pi's sound card (see section 7). Use <b>-a browser</b> to "
                    "use the browser's own microphone and speakers instead."],
    ], [1.7, 6.3])

    # ------------------------------------------------------------- 4. the page
    m.h1("4. Opening the interface")
    m.p("In a browser on the same network, go to the address of the Pi followed by the port number:")
    m.code("http://10.168.1.33:8080/")
    m.p("Substitute the address of your own Pi. Bookmark it: this is the whole interface.")
    m.p("At the top right there is an <b>online / offline</b> marker. If it says offline, the page has "
        "lost contact with the Pi and nothing you press will do anything \u2014 check that the program "
        "is running (section 3) and reload the page.")
    m.pagebreak()

    m.h1("5. The screen at a glance")
    m.sketch()
    m.table([
        ["Area", "What it is for"],
        ["PUSH TO TALK", "Bottom right. Press and hold while you speak, then release. You can hold the "
                         "space bar instead. A physical switch on the Pi does the same job."],
        ["RX / TX light", "Shows whether you are transmitting. Next to it, <b>in frame</b> means a valid "
                          "M17 signal is being decoded, <b>no signal</b> means nothing is there. In client "
                          "mode - where there is no radio and the audio comes from a reflector - the same "
                          "box names the station you are hearing, as <b>hearing G4ABC</b>."],
        ["signal and power bars", "The plain bars at the top of the PUSH TO TALK card: how strong the "
                                  "received signal is, and your transmit power setting. (The audio levels in "
                                  "the Audio card are the dials.)"],
        ["Station", "Your callsign, who you are calling, and the broadcast and log buttons."],
        ["Radio", "The frequencies and gain settings, and the button that applies them."],
        ["Reflector", "Linking over the internet to other M17 users (you may never need it)."],
        ["Audio", "Where your voice comes from and goes, the volume, and how healthy your transmit audio is."],
        ["Console", "A command box for advanced settings, and the log that shows what is happening."],
        ["locator, position, gnss (top of the page)",
         "Your Maidenhead locator and latitude/longitude from the GPS, and whether your position is being "
         "sent with your transmissions."],
    ], [1.9, 6.1])

    m.h2("Your position: the locator, and the compass")
    m.p("If a GPS receiver is plugged into the Pi, your position is read from it and shown in two places: "
        "in the header at the top (locator, latitude and longitude) and in the <b>Position</b> panel, in "
        "large letters.")
    m.table([
        ["What you see", "What it means"],
        ["IO90ST", "Your Maidenhead locator: six characters giving your position to about 5 km. "
                   "Other operators use these to aim antennas, and they are the polite way to say where "
                   "you are."],
        ["50.8036, -0.4419", "Latitude and longitude in degrees. South and west are negative."],
        ["satellites / HDOP / altitude", "How good the fix is. Six or more satellites and an HDOP below "
                                         "about 2 means a good one. Altitude is in metres."],
        ["copy locator", "Puts the locator on the clipboard, ready to paste into a log or a message."],
        ["position ready / position sent / position off air", "Whether your position is being sent with "
                                           "your transmissions. It is sent only while the fix is fresh, about "
                                           "every 5 seconds; it reads <b>position off air</b> when you have "
                                           "switched the sending off."],
        ["the compass", "The bearing to the station you are hearing, with the distance in kilometres. "
                        "The red needle points at them; the small yellow mark is the direction they say "
                        "they are travelling. It needs a fix of your own to work."],
        ["heard G4ABC ...", "The position another station sent, with its locator and how long ago it was "
                            "received."],
    ], [2.3, 5.7])
    m.note("Your own position is sent to other stations as part of the M17 link setup, so send it only if "
           "you are happy for it to be public - it appears in the Location column of the reflector's own "
           "dashboard, and in any client that displays it. If you would rather not, untick <b>announce "
           "position on air</b> in the Position panel, or give the command <b>gps meta off</b>. The receiver "
           "keeps working either way, so the locator, the compass and the position log are unaffected, and "
           "the setting is remembered when you next start the program. Unplugging the receiver also works, "
           "if you want no position at all.")

    m.h2("Keeping a log of your position")
    m.p("Tick <b>log to CSV</b> in the Position panel and a row is added to a file every few seconds, "
        "which you can open in a spreadsheet: date, time, latitude, longitude, locator, altitude, speed, "
        "course, satellites, HDOP, the age of the fix and a timestamp. The panel tells you where the file "
        "is \u2014 <b>gps_track.csv</b> in the program's folder on the Pi \u2014 and how many rows it holds. "
        "Untick it to stop. The <b>csv</b> command (section 9) does the same thing, and can change how "
        "often a row is written.")

    # -------------------------------------------------------- 6. making a contact
    m.h1("6. Making a contact")
    m.steps([
        "Type your callsign into <b>Station \u2192 my callsign</b>. It is sent with every transmission, "
        "so it must be correct.",
        "Agree a frequency with the other station, then type it into <b>Radio \u2192 RX frequency (Hz)</b> "
        "and press <b>apply frequencies</b>. This sets transmit to the same frequency.",
        "Listen first. When the other station speaks, the light shows <b>RX</b>, the signal bar moves, "
        "<b>in frame</b> appears and you hear them.",
        "To reply, hold <b>PUSH TO TALK</b>, speak in your normal voice a little slower than a "
        "telephone call, then release.",
        "Wait for the other station to finish before you press again \u2014 while you are transmitting, "
        "you cannot hear them.",
    ])
    m.note("Only transmit on frequencies your licence allows, and check what is already in use before "
           "you press. If you are testing, a dummy load or the lowest gain keeps you out of trouble.")

    # ----------------------------------------------------------------- 7. audio
    m.h2("Using a physical PTT switch")
    m.p("As well as the button on the page you can transmit with a switch wired to the Raspberry Pi's "
        "header \u2014 a hand microphone, a foot switch, or a small box you can hold. Pressing it "
        "transmits exactly as the button does, and the RX / TX light follows it, so you can put the "
        "browser down and operate normally.")
    m.table([
        ["Pin", "What it is for"],
        ["GPIO 11 (header pin 23)",
         "One side of the switch. The other side goes to ground \u2014 for example header pin 25 or 9."],
        ["GPIO 10 (header pin 19)",
         "An output that goes high while transmitting, to key an amplifier or another radio through a "
         "suitable interface."],
    ], [2.4, 5.6])
    m.note("Ask whoever set the system up before wiring anything to the header: the pins have to match, "
           "and the switch must go <b>to ground</b>, not to 3.3 V. Use a transistor or a relay for "
           "anything that draws real current \u2014 these are logic outputs, not a power supply. Note also "
           "that GPIO 10 and 11 are the SPI pins, so an SPI display would use them.")
    m.p("Releasing the switch returns you to receive. Releasing it does not cancel a transmission you "
        "started from the browser, so the two ways of transmitting do not fight each other.")

    m.h1("7. Audio \u2014 where your voice comes from and goes")
    m.p("The <b>Audio</b> panel has one button that matters: <b>Audio configuration&hellip;</b>. It "
        "opens a box with two sets of choices \u2014 one for the <b>speaker</b> and one for the "
        "<b>microphone</b> \u2014 and nothing changes until you press <b>Configure Audio</b>.")
    m.table([
        ["Choice", "What it means", "When to use it"],
        ["browser",
         "The microphone and speakers of the computer you are browsing from, carried over the network.",
         "When you are away from the Pi, or it has no sound card."],
        ["soundcard",
         "The microphone and speaker plugged into the Pi's sound card, talked to directly.",
         "<b>Recommended</b> when a card is attached: the most reliable choice."],
        ["bluetooth",
         "A Bluetooth speaker or headset, chosen with <b>Configure Bluetooth</b>.",
         "A tidy station with no wires. See below."],
        ["mute",
         "Nothing: silence is transmitted, and nothing is played.",
         "For testing, or to silence one direction only."],
    ], [1.6, 4.4, 2.0])
    m.p("The two choices are independent, so the speaker can be a Bluetooth speaker while the "
        "microphone stays on the sound card, or the browser can carry the sound while the Pi's card "
        "takes the microphone. Press <b>Configure Audio</b> and the station detaches whatever that "
        "direction was using, attaches what you asked for, and keeps the choice for the next start. "
        "<b>(applying&hellip;)</b> means it takes effect at the start of the next transmit/receive "
        "cycle \u2014 never in the middle of a transmission.")
    m.note("If you ask for <b>bluetooth</b> before a device has been chosen, nothing moves: the "
           "station says to use Configure Bluetooth first, and that direction stays where it was. "
           "That is deliberate \u2014 a half-made request must not silence the station.")
    m.h2("Configure Bluetooth")
    m.p("<b>Configure Bluetooth</b> opens the list of devices the station already knows. Each row "
        "has a <b>speaker</b> button and a <b>mic</b> button, there is a <b>scan</b> for new "
        "devices, and a <b>sound card</b> row at the top to put either direction back on the card.")
    m.p("Choosing a device here <b>remembers it and connects it, but does not move the audio</b>: "
        "press <b>Configure Audio</b> afterwards to attach it. That way a half-chosen request never "
        "takes the sound away.")
    m.p("The two buttons are independent, so the SoundCore mini can carry the sound (A2DP, playback "
        "only) while a headset carries the microphone (HFP, 8 kHz \u2014 exactly M17's own rate, so "
        "nothing is lost against Codec2). A headset that has no A2DP at all \u2014 the BH-M10B is "
        "one \u2014 shows <b>speaker (HFP)</b> instead of <b>speaker</b>, and its own earpiece "
        "carries the sound at 8 kHz; the station works that out from the device, so there is nothing "
        "to set. A device a scan has just found is only <i>discovered</i>: it shows <b>(not "
        "paired)</b> and a <b>pair</b> button instead, and nothing can connect it until it is "
        "paired.")
    m.p("If the device is switched off, or out of range, the station says so and keeps trying: it "
        "picks the device up by itself as soon as it is switched on, without a restart. With no "
        "sound card attached there is deliberately nothing to fall back to, so a Bluetooth-only "
        "station is a proper arrangement, not a fault. Bluetooth audio needs the "
        "<code>bluez-alsa</code> packages on the Pi; without them the box says so and everything "
        "else is unchanged.")
    m.h2("The two level controls, and the meters")
    m.p("The two meters are moving-coil instruments, not bars: a needle on a scale from -40 to "
        "+3 dB, the last three dB in red, a pale marker that holds the peak and falls back slowly, "
        "and a clip lamp in the corner that lights when the level reaches full scale (click it to "
        "reset). The needle is damped - quick to rise, slow to fall - so it is readable while you "
        "talk; the figure underneath is the exact level, and it turns amber above -6 dB and red at 0.")
    m.p("Beside the meters are <b>mic level</b> and <b>volume</b>. Both are applied on the Pi "
        "itself: the microphone gain sits between the microphone and the modulator, and the volume "
        "between the receiver and the speaker. That is what makes the meters worth watching \u2014 "
        "the <b>mic</b> bar shows the level actually going out on air, not just what a browser "
        "happened to capture, and the <b>spk</b> bar shows what is being played.")
    m.p("The readouts are in decibels. <b>mic level</b> runs from 0 (silent) to 200% (+6 dB); 100% "
        "is 0 dB and suits most microphones, so raise it if you sound quiet on air and lower it if "
        "the bar is pinned while you speak normally. <b>volume</b> runs 0-100%, with 100% at 0 dB.")
    m.p("Both are kept for the next start, and the console does the same: <b>micgain 150</b> and "
        "<b>spkvol 60</b>.")
    m.p("Received audio is played through about a third of a second of buffer, which is what keeps "
        "the odd late moment from being heard as a gap. If the status line under the meters ever "
        "reports <b>speaker underruns</b>, the card itself ran dry \u2014 that number is worth "
        "quoting to whoever looks after the station, because no underruns at all means any break-up "
        "is happening before the audio gets to the card.")
    m.h2("The card's own controls: the Mixer panel")
    m.p("A USB sound card has controls of its own \u2014 a volume, and often a microphone gain and "
        "an automatic-gain switch. These are <b>not</b> the same as the application's levels: they "
        "belong to the card, and one of them sitting in the wrong place is the usual reason the "
        "audio looks healthy while nothing can actually be heard. A C-Media headset, for instance, "
        "comes up at 29% (about -20 dB), which is quiet enough to lose received voice completely.")
    m.p("The <b>Mixer</b> panel lists whatever controls your card really has, and setting one takes "
        "effect at once. Whatever you leave them at is remembered and applied again every time the "
        "audio is opened \u2014 so a card that drops off USB and returns gets its levels back with "
        "it, and a reboot does not undo them. The console does the same: <b>mixer</b> lists the "
        "controls, and <b>mixer 0 100</b> sets one.")
    m.h2("If the browser refuses your microphone")
    m.p("Browsers only allow microphone access on a secure page. A page opened as "
        "<b>http://&lt;address&gt;:8080/</b> is not secure, so the microphone may be blocked. Either "
        "open <b>http://localhost:8080/</b> in a browser on the Pi itself, or choose the Pi's sound "
        "card instead \u2014 which avoids the problem completely and is what most users want. The "
        "<b>enable browser audio</b> button beside the configuration button starts the browser's "
        "audio by hand if a saved configuration needs it; Configure Audio does it for you otherwise.")
    m.h2("Blank frames before voice, and the transmit health line")
    m.p("<b>blank frames before voice</b> is the amount of silence sent at the start of each "
        "transmission, before your speech. It gives the receiving station time to lock onto your "
        "signal, so your first word is not clipped. The default is 4 (about 80 thousandths of a "
        "second). If people tell you the start of your first word is missing, set it to 6 or 8.")
    m.p("The line beside it \u2014 <b>transmit clean</b>, or <b>dropped N samples</b> \u2014 is the health of "
        "your transmit audio. <b>Transmit clean</b> means the Pi kept up with the radio for the whole "
        "transmission. If it reports dropped samples, raise the blank frames a little and try again; "
        "if it keeps reporting drops, the Pi is too busy (close other programs) or something needs "
        "looking at by whoever maintains the system.")
    m.pagebreak()

    # ------------------------------------------------------ 8. the panels in detail
    m.h1("8. The controls, panel by panel")
    m.h2("Station")
    m.table([
        ["Control", "What it does"],
        ["my callsign", "Your callsign. Sent with every transmission."],
        ["destination", "Who you are calling. Leave as ALL unless you are calling one specific station."],
        ["gateway", "Used when linking through a reflector or repeater (advanced)."],
        ["listen to all", "Receive everything on the frequency, whoever it is addressed to."],
        ["broadcast (ba)", "Address the transmission to everyone (the usual choice on a calling channel)."],
        ["help / clear log", "Lists every command in the log, or empties the log."],
    ], [1.9, 6.1])

    m.h2("Mode")
    m.p("How the station is wired up. <b>Radio</b> is the normal voice mode. <b>Loopback</b> feeds "
        "your own audio straight back, for practice with no radio or aerial. <b>Gateway</b> and "
        "<b>client</b> link the radio, or your audio, to a reflector over the internet.")
    m.table([
        ["Setting", "What it does"],
        ["radio (normal)", "Radio in, sound out. Use this for contacts on a frequency."],
        ["loopback (practice)", "Your own voice comes straight back, so you can check the microphone "
                                "and the settings without transmitting."],
        ["gateway", "The radio is bridged to a reflector: what you hear on frequency goes out to the "
                    "reflector, and what comes back from the reflector goes out on frequency."],
        ["client", "Your voice goes to the reflector over the internet, with no radio involved."],
    ], [1.9, 6.1])
    m.note("Changing the mode takes effect at once \u2014 you do not have to stop and start the "
           "program. If you are transmitting when you change it, the transmission stops. The choice "
           "is kept, so the same mode is still selected after a restart.")

    m.h2("Radio")
    m.p("The receive controls are stacked above the transmit ones.")
    m.table([
        ["Control", "What it does"],
        ["RX frequency (Hz)", "The frequency you listen on, in hertz (145500000 = 145.5 MHz)."],
        ["TX frequency (Hz)", "The frequency you transmit on. Usually the same as RX."],
        ["RX gain / TX gain", "Receive and transmit levels. Leave them as they are unless someone asks you to change them."],
        ["freq correction", "Fine tuning, for when a station sounds slightly off. Change it in small steps."],
        ["apply frequencies", "Press this after changing a frequency or a gain, or nothing changes."],
        ["transmit carrier", "Sends a plain carrier with no voice - for tests and alignment only."],
        ["receive", "Returns to receiving if a transmission is stuck on, or after a test."],
        ["noise", "Practice mode only: adds noise to the simulated signal."],
    ], [2.0, 6.0])

    m.h2("Reflector")
    m.p("Reflectors link M17 stations over the internet, so you can talk to people out of radio "
        "range. Type the reflector's name \u2014 they are written like <b>M17-M17</b> \u2014 or pick one "
        "from the list that appears as you type, choose the channel letter (the <b>module</b>), and "
        "press <b>connect</b>. Press <b>disconnect</b> when you have finished: it is good manners, "
        "and it frees the channel for others.")
    m.p("Watch the log while you do it. It either says which address it is connecting to, or it says "
        "that the name is not in the list \u2014 in which case check the spelling against the reflector's "
        "own web page. The badge next to the buttons shows whether you are linked.")
    m.note("Reflector addresses do change over time. If a reflector that used to work stops working, "
           "ask whoever maintains the system to refresh the list: the program reads it from a file on "
           "the Pi, and the file has to be brought up to date (there is a tool for it). A stale "
           "address looks exactly like a connect button that does nothing. If you only want local "
           "radio contacts, ignore this panel.")

    m.h2("Position")
    m.p("Your GPS position and Maidenhead locator in large letters, the compass showing where the "
        "station you are hearing is, and the tick box that keeps a log of your position in a file. "
        "Section 5 explains what each part means.")

    m.h2("Audio")
    m.table([
        ["Control", "What it does"],
        ["Audio configuration\u2026", "Where the speaker and the microphone each come from and go - see section 7."],
        ["enable browser audio", "Starts this page's own audio by hand, if a saved configuration needs it and Configure Audio has not been pressed."],
        ["mic / spk meters", "Two analogue dials: the microphone level going out and the speaker level being played, both measured on the Pi after its own level controls. The figure under each one is the exact level in dB."],
        ["mic level / volume", "Microphone gain and speaker volume, applied on the Pi - see section 7."],
        ["mixer", "The sound card's own volume and gain controls - see section 7."],
        ["blank frames before voice", "Silence sent before your speech at the start of a transmission. Default 4."],
    ], [2.3, 5.7])

    m.h2("Console")
    m.p("Every setting also has a typed command, and the console is where you type them. You do not "
        "need it for normal operation; the help button lists the commands, and section 9 lists them "
        "all. The log below it shows what the program is doing \u2014 useful when something does not "
        "work.")

    # ----------------------------------------------------------- 9. the commands
    m.h1("9. Commands")
    m.p("Typed into the Console box, or into the program's own terminal. Type one command per line.")
    m.table([
        ["Command", "What it does"],
        ["rf 145500000", "Set the receive frequency, in hertz."],
        ["tf 145500000", "Set the transmit frequency, in hertz."],
        ["rg 0.5 / tg 0.5", "Set the receive or transmit gain, from 0.0 to 1.0."],
        ["fc 1.0001", "Frequency correction, from 0.9 to 1.1."],
        ["afc on / off", "Automatic frequency control."],
        ["tx / rx", "Start or stop transmitting (the PTT button does the same)."],
        ["td / tc", "Transmit duplex, or transmit a plain carrier."],
        ["sa G4ABC", "Source address: your callsign."],
        ["da ALL", "Destination address: who you are calling."],
        ["ga G4ABC G", "Gateway address."],
        ["ba", "Broadcast: call everyone."],
        ["la on / off", "Listen to all addresses, or only yours."],
        ["conn NAME C", "Connect to a reflector, on module C."],
        ["disc", "Disconnect from the reflector."],
        ["mode radio", "Normal mode: radio in, sound out. Alternatives: <b>loop</b> (audio loops back for "
                       "practice), <b>gate</b> (radio to reflector), <b>client</b> (sound to reflector). "
                       "Takes effect at once, and is kept for the next start."],
        ["audio", "Which route the speaker and the microphone are on, and the two levels."],
        ["audio apply spk bluetooth mic soundcard",
         "What <b>Configure Audio</b> sends: attach these two routes. Each is <b>browser</b>, "
         "<b>soundcard</b>, <b>bluetooth</b> or <b>mute</b>. Kept in config.txt."],
        ["micgain 150", "Microphone gain as a percentage of unity (100 = 0 dB, 200 = +6 dB), applied "
                        "before the modulator."],
        ["spkvol 60", "Speaker volume 0-100% (100 = 0 dB), applied to received audio."],
        ["audio alsa", "Both directions at once, the old way: <b>browser</b>, <b>alsa</b>, <b>both</b>, "
                       "<b>none</b>, or <b>pulse</b>."],
        ["mixer", "List the sound card's own controls and what they are set to - see section 7."],
        ["mixer 0 100", "Set control number 0 to 100%. <b>on</b> and <b>off</b> also work, for a "
                        "switch. Kept in config.txt for the next start."],
        ["bt", "List the Bluetooth devices the station knows, and which is in use - see section 7."],
        ["bt sel spk MAC", "Choose and connect a speaker without moving the audio - what Configure "
                           "Bluetooth does. <b>bt sel mic MAC</b> is the microphone; <b>off</b> clears it."],
        ["bt spk MAC", "Choose a Bluetooth speaker (A2DP) <i>and</i> move the speaker output to it."],
        ["bt spk off", "Put the speaker output back on the sound card."],
        ["bt mic MAC", "Take the microphone from that Bluetooth headset (HFP)."],
        ["bt mic off", "Put the microphone back on the sound card."],
        ["bt scan", "Look for Bluetooth devices for 20 seconds."],
        ["bt pair MAC", "Pair and trust a device a scan found, before choosing it."],
        ["bt off", "Put the audio back on the Pi sound card."],
        ["lead 4", "Blank codec frames sent before the voice - see section 7."],
        ["fargan", "The neural receiver: whether FARGAN is built in, and whether it is on."],
        ["fargan on", "Receive through FARGAN at 16 kHz instead of the classic decoder. Costs about "
                      "half the Pi's CPU, and is remembered for the next start."],
        ["fargan off", "Back to the classic decoder."],
        ["record", "Is it recording, and where? Whatever you hear is what gets written."],
        ["record on", "Record what the receiver decodes as a WAV in the station's working "
                      "directory: 16 kHz when FARGAN is on, since that is what it synthesises, "
                      "and 8 kHz with the classic decoder. Not remembered across a restart, and "
                      "it stops itself after 30 minutes."],
        ["record off", "Close the recording; the file is then complete."],
        ["gps", "Show the GPS receiver, the fix and your locator. <b>gps /dev/ttyACM1 38400</b> uses a "
                "different receiver."],
        ["gps meta off", "Stop sending your position to other stations; <b>gps meta on</b> starts again. The "
                         "receiver keeps running, so the locator, the compass and the log are unaffected - "
                         "see section 5."],
        ["csv on / csv off", "Start or stop logging your position to a file - see section 5."],
        ["csv 10", "Log a row every 10 seconds rather than every 5."],
        ["csv status", "Show the log file and how many rows it holds."],
        ["load FILE", "Run a list of commands from a file."],
        ["w", "Clear the log pane."],
        ["b", "Break: stop transmitting now."],
        ["q", "Quit the program."],
        ["h", "List the commands."],
    ], [1.7, 6.3])

    # --------------------------------------------------------- 10. troubleshooting
    m.h1("10. If something goes wrong")
    m.table([
        ["What you see", "What to try"],
        ["The page will not open at all",
         "Check the address and the port (8080), and that the Pi is switched on and on the network. "
         "On the Pi, run <b>bash tools/pi_m17web.sh status</b>."],
        ["It says <b>offline</b> at the top",
         "The page lost contact with the Pi. Reload the page. If it stays offline, check the program "
         "is still running on the Pi."],
        ["Nothing comes out of the speaker",
         "Press <b>Audio configuration\u2026</b> and check which route the speaker is on, that it is "
         "not <b>mute</b>, and that <b>volume</b> is up. On <b>soundcard</b>, check the speaker is "
         "plugged into the sound card (not the Pi's headphone socket) and look at the Mixer panel."],
        ["I chose bluetooth but the audio did not move",
         "Configure Bluetooth only chooses and connects a device; press <b>Configure Audio</b> to "
         "attach it. If no device had been chosen, the station says so and leaves that direction "
         "where it was."],
        ["The other station says my audio breaks up, or starts late",
         "Look at the transmit health line in the Audio panel. Raise <b>blank frames before voice</b> "
         "to 6-8 and try again. If it still reports dropped samples, the Pi is struggling with "
         "something else running."],
        ["I hear nothing when the other station speaks",
         "Check you are on the same frequency, and that <b>in frame</b> appears when they talk. If the "
         "signal bar moves but there is no audio, try <b>afc on</b>, then nudge <b>freq correction</b> "
         "slightly."],
        ["The browser will not use my microphone",
         "Browsers only allow the microphone on a secure page. Use <b>http://localhost:8080/</b> on "
         "the Pi itself, or route the microphone to <b>soundcard</b> or <b>bluetooth</b> in the "
         "Audio configuration box (section 7)."],
        ["The light never goes to TX",
         "If the header says offline, the page cannot reach the Pi. Otherwise something else may be "
         "using the radio \u2014 stop and start the program."],
        ["The sound card is reported as busy",
         "Another program (or an earlier run) is using it. Stop other audio programs, then stop and "
         "restart m17web."],
        ["The program will not start (port already in use)",
         "An older copy is still running: <b>bash tools/pi_m17web.sh stop</b>, wait a moment, then "
         "start it again."],
        ["I transmit but nobody hears me",
         "Test your own audio first, with no second operator needed: set the <b>destination</b> to "
         "<b>PARROT</b> and transmit for a few seconds. The reflector echoes your voice straight back, "
         "so if you hear yourself, your station is fine and the problem is at the far end (a listener "
         "on the wrong module, or a browser that has not been allowed to play audio). Set the "
         "destination back to <b>ALL</b>, or press <b>broadcast (ba)</b>, when you have finished."],
        ["The reflector will not connect",
         "Type the name exactly as the reflector publishes it (<b>M17-M17</b>, not the short form), "
         "press connect, and read the log: it says which address it is trying, or that the name is "
         "not in the list. If the list is out of date, ask whoever maintains the system to refresh "
         "it. Some reflectors only answer on particular modules."],
        ["The locator is blank, or there is no position",
         "The GPS receiver has not got a fix yet, or is not plugged in. A fix can take a minute or two "
         "outdoors, and from inside a building it may never arrive. Nothing else needs it, so carry on."],
        ["The physical PTT switch does nothing",
         "Check the wiring first (section 6). On the Pi, <b>gpioinfo</b> should list GPIO 10 and 11 as "
         "used by m17web. If it shows something else, or the panel reports no hardware PTT, ask whoever "
         "maintains the system."],
        ["The radio cannot be found at all",
         "The Pluto re-appears as a USB network interface when it is plugged in, and it needs the "
         "address 192.168.2.2. Check it is connected, then ask whoever maintains the system \u2014 this "
         "is covered in the README."],
    ], [2.1, 5.9])

    # ------------------------------------------------------------ 11. good practice
    m.h1("11. Good practice")
    m.bullets([
        "Transmit only on frequencies your licence covers, and at the lowest power that works.",
        "Listen before you transmit, and give way to a station already in contact.",
        "Keep your callsign correct in the Station panel \u2014 it is how other operators know who is calling.",
        "When using a reflector, say where you are going and disconnect when you have finished.",
        "For tests, use a dummy load or a carrier into a low gain rather than transmitting into the antenna.",
        "If you leave the system running, mute the audio if you do not want your room heard on the air.",
    ])

    # --------------------------------------------------------- 12. quick reference
    m.pagebreak()
    m.h1("12. Quick reference")
    m.h2("Start it")
    m.code("cd /mnt/hf/shared/projects/m17_sdr_web/m17gismo<br/>"
           "RADIO=pluto MODE=radio bash tools/pi_m17web.sh start -a alsa")
    m.h2("Open it")
    m.code("http://10.168.1.33:8080/     (your own Pi's address)")
    m.h2("Make a contact")
    m.table([
        ["Step", "Do this"],
        ["1", "Station → my callsign: type your callsign."],
        ["2", "Radio → RX frequency (Hz): type the agreed frequency, press <b>apply frequencies</b>."],
        ["3", "Listen until you hear the other station (<b>in frame</b> appears)."],
        ["4", "Press and hold <b>PUSH TO TALK</b> (bottom right), speak, then release."],
        ["5", "Let the other station finish before pressing again."],
    ], [0.9, 7.1])
    m.h2("The three things that put most people right")
    m.table([
        ["If", "Then"],
        ["You cannot hear anything",
         "Check the frequency, and whether <b>in frame</b> appears when the other station speaks. "
         "Try <b>afc on</b>."],
        ["Nobody can hear you",
         "Look at the transmit line in the Audio panel. If it reports dropped samples, raise "
         "<b>blank frames before voice</b> to 6-8."],
        ["The page will not reach the Pi",
         "On the Pi: <b>bash tools/pi_m17web.sh status</b>, then reload the page."],
    ], [2.3, 5.7])
    m.note("Your locator and position are in the header at the top of the page; <b>log to CSV</b> in "
           "the Position panel keeps a track of where you have been. A switch wired to the Pi transmits "
           "just as the button does. Settings worth knowing: <b>audio device</b> (use the Pi's sound card, direct), "
           "<b>blank frames before voice</b> (default 4) and <b>volume</b> in the Audio panel; "
           "<b>help</b> in the Station panel lists every command.")

    # --------------------------------------------------------------- 13. credits
    m.p("The optional neural receiver is <b>Wojciech Kaczmarski SP5WWP</b>'s "
        "Codec2-mod-neural project, which synthesises the same Codec2 3200 frames with FARGAN - "
        "the vocoder from the Opus codec, by Xiph.Org Foundation - instead of the classic "
        "sinusoidal decoder. Codec2 itself is David Rowe VK5DGR's.")

    m.h1("13. Where this came from, and further reading")
    m.p("m17web is a modification of <b>G4GUO/m17_sdr</b>, with the original terminal interface "
        "replaced by the web page described here. The M17 protocol it speaks is an open standard "
        "from the M17 Project; the voice coding is Codec2 by David Rowe and the M17 Project's "
        "Codec2-mod build. It uses the open-source libiio, ALSA, Codec2 and libgpiod libraries.")
    m.p("For the technical details \u2014 building it yourself, the audio back ends, the transmit "
        "timing and the rate planning \u2014 see <b>README.md</b> next to this manual, in the same folder.")
    m.note("The upstream project has no licence file, so this fork adds its own documentation only. "
           "Treat the software as you find it: it works, but check it against your own licence "
           "conditions before putting it on the air.")

    # ------------------------------------------------------------------ notes
    m.pagebreak()
    m.h1("Notes")
    m.p("Frequencies in use locally, reflector names, who to call when it breaks:")
    for _ in range(18):
        m.story.append(Spacer(1, 0.42 * cm))
        m.story.append(HRFlowable(width="100%", thickness=0.4,
                                  color=colors.HexColor("#c8d2dc"), spaceAfter=0))

    # ------------------------------------------------------------------ render
    doc = BaseDocTemplate(path, pagesize=A4, title=TITLE, author="m17web project",
                      leftMargin=MARGIN, rightMargin=MARGIN, topMargin=1.8 * cm,
                      bottomMargin=1.8 * cm)

    def on_page(canv, _doc):
        canv.saveState()
        canv.setStrokeColor(colors.HexColor("#c8d2dc"))
        canv.setLineWidth(0.4)
        y = A4[1] - 1.35 * cm
        canv.line(MARGIN, y, A4[0] - MARGIN, y)
        canv.setFont("Helvetica", 8)
        canv.setFillColor(colors.HexColor("#77808a"))
        canv.drawString(MARGIN, y + 4, TITLE)
        canv.drawRightString(A4[0] - MARGIN, y + 4, "M17 digital voice over SDR")
        canv.line(MARGIN, 1.35 * cm, A4[0] - MARGIN, 1.35 * cm)
        canv.drawCentredString(A4[0] / 2.0, 1.0 * cm, "Page %d" % canv.getPageNumber())
        canv.restoreState()

    frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="body")
    doc.addPageTemplates([PageTemplate(id="all", frames=[frame], onPage=on_page)])
    doc.build(m.story)
    print("wrote", path)


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "docs/m17web-user-manual.pdf")
