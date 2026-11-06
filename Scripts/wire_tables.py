# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""wire_tables.py — the wire's numbers as the C++ headers declare them.

A client written in C++ includes the headers. The Emacs and Neovim clients
cannot, so the numbers they need are written out for them from the headers:
`Scripts/gen-wire-tables.py` writes one Emacs Lisp file and one Lua file, both
checked in, and `Scripts/check-wire-tables.py` fails when a checked-in file is
not what the headers produce now. Neither client types a wire number of its
own for anything listed in `SCALARS`, `ENUMERATIONS` and the functions below.

This module is the reader both scripts and `check-wire-version-pins.py` share:
a constant's value, an enumeration's members, which frame kinds only a client
sends, a new terminal's interrupt characters. It reads declarations with
regular expressions, so it reads the forms these headers use and reports any
it cannot (`Unreadable`), instead of guessing a value.

Stdlib only.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))

ADMISSION_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Admission.h")
ENVELOPE_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Envelope.h")
ENVELOPE_SOURCE = os.path.join("Source", "Core", "Protocol", "Sync", "Envelope.cpp")
MESSAGES_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Messages.h")
SCREEN_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "ScreenPlane.h")
ROW_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "RowForm.h")
CONNECTION_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Connection.h")
KEYS_HEADER = os.path.join("Source", "Core", "Keys", "KeyTranslation.h")
KEYMAP_HEADER = os.path.join("Source", "Core", "Keys", "Keymap.h")
ATTRIBUTE_HEADER = os.path.join("Source", "Core", "VT", "Constants.h")
COLOR_HEADER = os.path.join("Source", "Core", "Color.h")
ROW_META_HEADER = os.path.join("Source", "Core", "RowMeta.h")
TEXT_SIZE_HEADER = os.path.join("Source", "Core", "TextSizeTable.h")
PALETTE_HEADER = os.path.join("Source", "Core", "TermPalette.h")
RECORDS_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Records.h")
TERMINAL_HEADER = os.path.join("Source", "Core", "Terminal.h")
TAB_STATUS_HEADER = os.path.join("Source", "Core", "OSC", "TabStatus.h")
PROGRESS_HEADER = os.path.join("Source", "Core", "OSC", "ProgressIndicator.h")
IMAGE_HEADER = os.path.join("Source", "Core", "Image", "TerminalImage.h")
SYNC_IMAGES_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Images.h")
EFFECTS_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Effects.h")
ROLES_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Roles.h")
EFFECT_CODEC_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "EffectCodec.h")
NOTIFICATION_HEADER = os.path.join("Source", "Core", "OSC", "KittyNotification.h")
SYNTH_HEADER = os.path.join("Source", "Core", "VT", "DECPSSynth.h")
SOUND_HEADER = os.path.join("Source", "Core", "VT", "Sound.h")
SAMPLES_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "Samples.h")
PAL_HEADER = os.path.join("Source", "PAL", "PAL.h")
CONSENT_STATUS_HEADER = os.path.join("Source", "Core", "Protocol", "Sync", "ConsentStatus.h")
TERMINAL_CLIENTS_HEADER = os.path.join("Source", "PAL", "TerminalClients.h")
NAME_LIMITS_HEADER = os.path.join("Source", "Core", "Session", "NameLimits.h")

# Every file the tables are read from, in the order the outputs cite them.
SOURCES = (ADMISSION_HEADER, ENVELOPE_HEADER, ENVELOPE_SOURCE, MESSAGES_HEADER, SCREEN_HEADER, ROW_HEADER,
           CONNECTION_HEADER, KEYS_HEADER, KEYMAP_HEADER, ATTRIBUTE_HEADER, COLOR_HEADER, ROW_META_HEADER,
           TEXT_SIZE_HEADER, PALETTE_HEADER, RECORDS_HEADER, TERMINAL_HEADER, TAB_STATUS_HEADER, PROGRESS_HEADER,
           IMAGE_HEADER, SYNC_IMAGES_HEADER, EFFECTS_HEADER, ROLES_HEADER, EFFECT_CODEC_HEADER, NOTIFICATION_HEADER, SYNTH_HEADER,
           SOUND_HEADER, SAMPLES_HEADER, PAL_HEADER, TERMINAL_CLIENTS_HEADER, CONSENT_STATUS_HEADER, NAME_LIMITS_HEADER)

EMACS_OUTPUT = os.path.join("Source", "Platform", "Emacs", "ubiquitty-sync-wire.el")
NEOVIM_OUTPUT = os.path.join("Source", "Platform", "Neovim", "lua", "ubiquitty_sync", "wire.lua")
GENERATOR = "Scripts/gen-wire-tables.py"


class Unreadable(Exception):
    """A declaration the tables need could not be read from its header."""


def read(path):
    """A repository file's text, empty when it cannot be read."""
    try:
        with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def sources():
    """The files the tables are read from, as {path: text}."""
    return {path: read(path) for path in SOURCES}


# ---- reading C++ ---------------------------------------------------------------

# Any named constant, and the expression it is worth. Parentheses are excluded
# from the value: a constant computed by a call is one this reader cannot
# evaluate, and it is absent rather than guessed.
CONSTANT_FORM = re.compile(r"(?:inline\s+)?constexpr\s+[\w:]+\s+(\w+)\s*=\s*([^;()]+);")

# An integer literal with C++ suffixes and digit separators, decimal or hex.
LITERAL_FORM = re.compile(r"(0[xX][0-9a-fA-F']+|\d[\d']*)[uUlL]*\Z")

# `std::size_t { 8 }`: a literal given its type, worth the literal.
TYPED_LITERAL_FORM = re.compile(r"[\w:]+\s*\{\s*([^{}]+?)\s*\}")


def withoutComments(text):
    """C++ text with its comments removed, so a number in prose is not read."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def literal(text):
    """An integer literal's value, or None when `text` is not one."""
    found = LITERAL_FORM.match(text.strip())
    return int(found.group(1).replace("'", ""), 0) if found else None


def product(expression, known):
    """A product of integer literals and resolved names, or None."""
    total = 1
    for term in expression.split("*"):
        term = term.strip()
        value = literal(term)
        if value is None:
            name = term.split("::")[-1]
            if not name.isidentifier() or name not in known:
                return None
            value = known[name]
        total *= value
    return total


def evaluate(expression, known):
    """A constant's value: sums of products, optionally shifted left.

    - Returns: the int, or None when a term is neither a literal nor a known
      name, or the expression has another shape.
    """
    expression = TYPED_LITERAL_FORM.sub(lambda found: found.group(1), expression)
    if "{" in expression or "}" in expression:
        return None
    parts = expression.split("<<")
    if len(parts) > 2:
        return None
    def summedProducts(text):
        values = [product(term, known) for term in text.split("+")]
        return None if any(value is None for value in values) else sum(values)
    value = summedProducts(parts[0])
    if value is None or len(parts) == 1:
        return value
    shift = summedProducts(parts[1])
    return None if shift is None else value << shift


def headerConstants(texts):
    """Every named constant these headers define, as {name: value}.

    An alias is resolved through the file it points into, so the pass repeats
    until nothing new resolves. A constant whose expression stays unresolved is
    absent rather than guessed.
    """
    pending = {}
    for text in texts:
        for name, expression in CONSTANT_FORM.findall(withoutComments(text)):
            pending.setdefault(name, expression.strip())
    known = {}
    resolving = True
    while resolving:
        resolving = False
        for name, expression in list(pending.items()):
            value = evaluate(expression, known)
            if value is None:
                continue
            known[name] = value
            del pending[name]
            resolving = True
    return known


def cppEnum(text, name):
    """An enumeration's members in order, as [(member, value)].

    A member with no initialiser is one more than the member before it, as the
    language counts; an initialiser is an integer or a shift of one. Scoped
    and unscoped enumerations both read.

    - Returns: the members, empty when the enumeration is not in `text` or a
      member's initialiser is neither form.
    """
    found = re.search(r"enum\s+(?:class\s+|struct\s+)?%s\b[^{;]*\{(.*?)\}\s*;" % re.escape(name), withoutComments(text), re.S)
    if not found:
        return []
    members = []
    following = 0
    for entry in found.group(1).split(","):
        entry = entry.strip()
        if not entry:
            continue
        member, _, initialiser = (part.strip() for part in entry.partition("="))
        if not member.isidentifier():
            return []
        if initialiser:
            shift = re.match(r"(\d+)[uU]?\s*<<\s*(\d+)\Z", initialiser)
            value = literal(initialiser)
            if shift:
                following = int(shift.group(1)) << int(shift.group(2))
            elif value is not None:
                following = value
            else:
                return []
        members.append((member, following))
        following += 1
    return members


def clientToHostKinds(source):
    """The frame kinds `directionOf` says only a client sends, by name."""
    body = re.search(r"Direction\s+directionOf\([^)]*\)\s*\{(.*?)\n\}", withoutComments(source), re.S)
    if not body:
        return set()
    head = body.group(1).split("return Direction::ClientToHost")[0]
    return set(re.findall(r"case\s+FrameKind::(\w+)\s*:", head))


def interruptDefaults(text):
    """A new terminal's interrupt, quit and suspend characters, in that order.

    - Returns: the three numbers, empty when `terminalDefaults` is not in
      `text` in the form it is written in.
    """
    found = re.search(r"terminalDefaults\(\)[^{]*\{\s*return\s*\{\s*\.interrupt\s*=\s*(\w+)\s*,\s*\.quit\s*=\s*(\w+)\s*,"
                      r"\s*\.suspend\s*=\s*(\w+)", withoutComments(text))
    return [int(value, 0) for value in found.groups()] if found else []


def connectionKinds(source):
    """The kinds whose canonical envelope channel is zero."""
    body = re.search(r"bool\s+isConnectionKind\([^)]*\)\s*\{(.*?)\n\}", withoutComments(source), re.S)
    if not body:
        return set()
    head = body.group(1).split("return true")[0]
    return set(re.findall(r"case\s+FrameKind::(\w+)\s*:", head))


# ---- names -----------------------------------------------------------------------

def words(member):
    """`PaneBind`, `colorRGB` and `QUICTokenGrant` as their lower-case words."""
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", " ", member).lower().split()


def snake(member):
    """`PaneBind` as `pane_bind`."""
    return "_".join(words(member))


def kebab(member):
    """`PaneBind` as `pane-bind`."""
    return "-".join(words(member))


def camel(name):
    """`paste_abort` and `paste-abort` as `PasteAbort`."""
    return "".join(part[:1].upper() + part[1:] for part in re.split(r"[-_]", name) if part)


# ---- what is written out ---------------------------------------------------------

# One number each: (Lua key, Emacs name after `ubiquitty-sync--`, header, the
# constant or `Enumeration::member`, what it is).
SCALARS = (
    ("max_consent_status_bytes", "max-consent-status-bytes", CONSENT_STATUS_HEADER, "maxConsentStatusBytes", "Own-consent status Notify body bytes"),
    ("max_panes_per_connection", "max-panes-per-connection", MESSAGES_HEADER, "maxPanesPerConnection", "Maximum bound pane handles on one connection."),
    ("first_listed_handle", "first-listed-handle", MESSAGES_HEADER, "firstListedHandle", "First host-listing handle; bound panes use lower nonzero handles."),
    ("max_effect_records", "max-effect-records", EFFECTS_HEADER, "maxEffectRecords", "Maximum effects in one delivery batch."),
    ("max_effect_clipboard_items", "max-effect-clipboard-items", EFFECT_CODEC_HEADER, "maxEffectClipboardItems", "Maximum MIME items in one clipboard operation."),
    ("max_effect_clipboard_mime_bytes", "max-effect-clipboard-mime-bytes", EFFECT_CODEC_HEADER, "maxEffectClipboardMIMEBytes", "Maximum bytes in one clipboard MIME name."),
    ("max_effect_clipboard_write_bytes", "max-effect-clipboard-write-bytes", EFFECT_CODEC_HEADER, "maxEffectClipboardWriteBytes", "Maximum clipboard-write data bytes."),
    ("max_effect_clipboard_read_bytes", "max-effect-clipboard-read-bytes", EFFECT_CODEC_HEADER, "maxEffectClipboardReadBytes", "Maximum clipboard-read data bytes."),
    ("max_notification_identifier_bytes", "max-notification-identifier-bytes", NOTIFICATION_HEADER, "kittyNotificationIdentifierCapacity", "Maximum notification replacement identifier bytes."),
    ("max_command_line_bytes", "max-command-line-bytes", TERMINAL_HEADER, "maxCommandLineLength", "Maximum command-finished command line bytes."),
    ("max_tone_notes", "max-tone-notes", PAL_HEADER, "maxNotes", "Maximum notes in one sound effect."),
    ("tone_default_envelope", "tone-default-envelope", PAL_HEADER, "waveDefaultEnvelope", "Sound envelope sentinel for the selected voice."),
    ("tone_channel_count", "tone-channel-count", SYNTH_HEADER, "toneChannelCount", "Number of sound channels, including the terminal speaker."),
    ("max_tone_wave", "max-tone-wave", SYNTH_HEADER, "ToneWave::Sample", "Highest admitted sound wave."),
    ("max_tone_volume", "max-tone-volume", SYNTH_HEADER, "toneMaxVolume", "Maximum volume and sustain level."),
    ("max_tone_envelope_milliseconds", "max-tone-envelope-milliseconds", SYNTH_HEADER, "toneMaxEnvelopeMilliseconds", "Maximum sound envelope duration."),
    ("max_tone_semitones_down", "max-tone-semitones-down", SYNTH_HEADER, "toneMaxSemitonesDown", "Maximum sound pitch offset."),
    ("max_tone_duration_units", "max-tone-duration-units", SYNTH_HEADER, "toneMaxDurationUnits", "Maximum sound duration in thirty-seconds of a second."),
    ("max_tone_effect", "max-tone-effect", SYNTH_HEADER, "toneMaxEffect", "Maximum sound note effect number."),
    ("max_tone_cutoff_hertz", "max-tone-cutoff-hertz", SYNTH_HEADER, "toneMaxCutoffHertz", "Maximum sound filter cutoff."),
    ("max_tone_pan", "max-tone-pan", SYNTH_HEADER, "toneMaxPan", "Maximum sound stereo position."),
    ("sample_slot_count", "sample-slot-count", SOUND_HEADER, "sampleSlotCount", "Number of downloadable sound sample slots."),
    ("sample_max_frames", "sample-max-frames", SOUND_HEADER, "sampleMaxFrames", "Maximum frames in one sound sample."),
    ("sample_rate_hertz", "sample-rate-hertz", SOUND_HEADER, "sampleRateHertz", "Sound sample frames per second."),
    ("sample_identity_bytes", "sample-identity-bytes", SAMPLES_HEADER, "sampleIdentityBytes", "Exact encoded sound sample identity size."),
    ("sample_head_bytes", "sample-head-bytes", SAMPLES_HEADER, "sampleHeadBytes", "Exact encoded sample announcement size."),
    ("max_progress_state", "max-progress-state", PROGRESS_HEADER, "ProgressState::ProgressPaused", "Highest pane progress state."),
    ("max_title_bytes", "max-title-bytes", TERMINAL_HEADER, "maxTitleLength", "Maximum pane title bytes."),
    ("max_directory_bytes", "max-directory-bytes", TERMINAL_HEADER, "maxCurrentDirectoryLength", "Maximum pane directory bytes."),
    ("max_file_url_bytes", "max-file-url-bytes", TERMINAL_HEADER, "maxCurrentFileURLLength", "Maximum pane file URL bytes."),
    ("max_home_bytes", "max-home-bytes", RECORDS_HEADER, "maxHomeDirectoryBytes", "Maximum host home directory bytes."),
    ("max_process_chain_bytes", "max-process-chain-bytes", RECORDS_HEADER, "maxProcessChainBytes", "Maximum process chain bytes."),
    ("max_process_arguments_bytes", "max-process-arguments-bytes", RECORDS_HEADER, "maxProcessChainWithArgumentsBytes", "Maximum process chain with arguments bytes."),
    ("max_process_name_bytes", "max-process-name-bytes", RECORDS_HEADER, "maxProcessNameBytes", "Maximum process or host name bytes."),
    ("max_indicator_bytes", "max-indicator-bytes", TAB_STATUS_HEADER, "tabStatusTextCapacity", "Maximum indicator label bytes."),
    ("max_badge_bytes", "max-badge-bytes", TAB_STATUS_HEADER, "badgeTextCapacity", "Maximum badge bytes."),
    ("max_attention_bytes", "max-attention-bytes", TAB_STATUS_HEADER, "attentionTextCapacity", "Maximum notification field bytes."),
    ("max_user_variables", "max-user-variables", TERMINAL_HEADER, "userVariableCapacity", "Maximum pane user variables."),
    ("max_user_name_bytes", "max-user-name-bytes", TERMINAL_HEADER, "maxUserVarNameLength", "Maximum user variable name bytes."),
    ("max_user_value_bytes", "max-user-value-bytes", TERMINAL_HEADER, "maxUserVarValueLength", "Maximum user variable value bytes."),
    ("wire_version", "wire-version", ENVELOPE_HEADER, "wireVersion",
     "The Sync wire version these tables were written at."),
    ("max_envelope_header", "max-envelope-header", ENVELOPE_HEADER, "maxEnvelopeHeaderBytes",
     "Maximum canonical envelope header bytes."),
    ("max_channel", "max-channel", ENVELOPE_HEADER, "maxChannel", "Maximum canonical pane handle."),
    ("max_app_version_bytes", "max-app-version-bytes", ADMISSION_HEADER, "maxAppVersionBytes", "Maximum app version bytes."),
    ("max_credential_bytes", "max-credential-bytes", ADMISSION_HEADER, "maxCredentialBytes", "Maximum credential bytes."),
    ("max_agent_label_bytes", "max-agent-label-bytes", ADMISSION_HEADER, "maxAgentLabelBytes", "Maximum presented claimed label bytes."),
    ("max_workspace_name_bytes", "max-workspace-name-bytes", NAME_LIMITS_HEADER, "maxWorkspaceNameBytes", "Maximum workspace name bytes."),
    ("max_host_platform_bytes", "max-host-platform-bytes", ADMISSION_HEADER, "maxHostPlatformBytes", "Maximum host platform bytes."),
    ("max_daemon_socket_name_bytes", "max-daemon-socket-name-bytes", ADMISSION_HEADER, "maxDaemonSocketNameBytes", "Maximum daemon socket basename bytes."),
    ("known_serving_build_flags", "known-serving-build-flags", ADMISSION_HEADER, "knownServingBuildFlags", "Known serving-build flag bits."),
    ("max_frame_body", "max-frame-body", ENVELOPE_HEADER, "maxFrameBodyBytes",
     "The longest body one Sync frame carries."),
    ("max_image_count", "max-image-count", IMAGE_HEADER, "maxTerminalImageCount",
     "Maximum image bindings in one pane."),
    ("max_image_placements", "max-image-placements", IMAGE_HEADER, "maxTerminalImagePlacementCount",
     "Maximum image placements in one pane."),
    ("image_binding_record_bytes", "image-binding-record-bytes", SYNC_IMAGES_HEADER, "imageBindingRecordBytes",
     "Bytes of one ImageBinding in an ImagePlacements body."),
    ("image_placement_record_bytes", "image-placement-record-bytes", SYNC_IMAGES_HEADER, "imagePlacementRecordBytes",
     "Bytes of one ImagePlacement in an ImagePlacements body."),
    ("max_image_bytes", "max-image-bytes", IMAGE_HEADER, "maxDecodedImageBytes",
     "Maximum decoded pixel bytes in one image."),
    ("max_image_dimension", "max-image-dimension", IMAGE_HEADER, "maxDecodedImageDimension",
     "Maximum width or height of a decoded image."),
    ("image_pixel_budget", "image-pixel-budget", IMAGE_HEADER, "terminalImageBytesBudgetDefault",
     "Default retained image-pixel budget of one terminal."),
    ("paste_piece_bytes", "paste-piece-bytes", CONNECTION_HEADER, "clientPastePieceBytes",
     "The most of a paste a client hands its transport at once."),
    ("max_probe_steps", "max-probe-steps", MESSAGES_HEADER, "maxProbeSteps",
     "The most steps a Welcome's probe schedule names."),
    ("pen_wire", "pen-wire", ROW_HEADER, "penWireBytes",
     "Bytes a Sync pen occupies."),
    ("state_block_count", "state-block-count", SCREEN_HEADER, "stateBlockCount",
     "The state blocks a checkpoint carries."),
    ("text_size_table_capacity", "text-size-table-capacity", TEXT_SIZE_HEADER, "textSizeTableCapacity",
     "Slots a text-size table always carries."),
    ("text_size_spec_wire_bytes", "text-size-spec-wire-bytes", SCREEN_HEADER, "textSizeSpecWireBytes",
     "Bytes one text-size slot occupies in the text-state block."),
    ("text_size_spec_proportional_offset", "text-size-spec-proportional-offset", SCREEN_HEADER, "textSizeSpecProportionalOffset",
     "Where a text-size slot's proportional flag sits within it."),
    ("palette_special", "palette-special", PALETTE_HEADER, "SpecialColor::specialCount",
     "Special palette colours per table."),
    ("color_default", "color-default", COLOR_HEADER, "colorDefault",
     "Pen colour index meaning the terminal default."),
    ("color_rgb", "color-rgb", COLOR_HEADER, "colorRGB",
     "Pen colour index meaning the direct RGB bytes."),
    ("line_double_width", "line-double-width", ROW_META_HEADER, "lineAttributeDoubleWidth",
     "Row attribute: DECDWL double width."),
    ("line_double_height_top", "line-double-height-top", ROW_META_HEADER, "lineAttributeDoubleHeightTop",
     "Row attribute: DECDHL top half."),
    ("line_double_height_bottom", "line-double-height-bottom", ROW_META_HEADER, "lineAttributeDoubleHeightBottom",
     "Row attribute: DECDHL bottom half."),
)

# Numbers written in hexadecimal, as their headers write them.
HEXADECIMAL = {"color_default", "color_rgb", "attribute"}

# One number per member: (Lua table, Emacs prefix after `ubiquitty-sync--`,
# header, enumeration, what a member is).
ENUMERATIONS = (
    ("role", "role", ROLES_HEADER, "Role", "Granted connection role"),
    ("client_kind", "client-kind", MESSAGES_HEADER, "ClientKind", "Verified client kind"),
    ("attachment_admission", "attachment-admission", MESSAGES_HEADER, "AttachmentAdmission", "Welcome consent admission"),
    ("resume_outcome", "resume-outcome", MESSAGES_HEADER, "ResumeOutcome", "A resumed pane's outcome"),
    ("bye_reason", "bye-reason", MESSAGES_HEADER, "ByeReason", "Canonical connection close reason"),
    ("effect_kind", "effect-kind", EFFECTS_HEADER, "EffectKind", "Effect kind"),
    ("notification_occasion", "notification-occasion", TERMINAL_CLIENTS_HEADER, "NotificationOccasion", "Notification presentation condition"),
    ("clipboard_buffer", "clipboard-buffer", TERMINAL_CLIENTS_HEADER, "ClipboardBuffer", "Clipboard selector"),
    ("effect_clipboard_request", "effect-clipboard-request", EFFECT_CODEC_HEADER, "EffectClipboardRequestKind", "Clipboard request kind"),
    ("effect_clipboard_status", "effect-clipboard-status", EFFECT_CODEC_HEADER, "EffectClipboardStatus", "Clipboard response status"),
    ("admission_target", "admission-target", ADMISSION_HEADER, "AdmissionTarget", "Canonical admission target"),
    ("kind", "kind", ENVELOPE_HEADER, "FrameKind", "Sync frame kind"),
    ("notify_kind", "notify-kind", ENVELOPE_HEADER, "NotifyKind", "Notify kind"),
    ("consent_status_phase", "consent-status-phase", CONSENT_STATUS_HEADER, "ConsentStatusPhase", "Own-consent status phase"),
    ("capability", "capability", MESSAGES_HEADER, "ClientCapability", "Greet capability bit"),
    ("view_state", "view", MESSAGES_HEADER, "ViewState", "View state"),
    ("resync_reason", "resync", MESSAGES_HEADER, "ResyncReason", "Resync reason"),
    ("paste_state", "paste-state", MESSAGES_HEADER, "PasteState", "Paste piece state"),
    ("block", "block", SCREEN_HEADER, "StateBlock", "State block"),
    ("attribute", "attr", ATTRIBUTE_HEADER, "Attribute", "Cell attribute bit"),
)

# Members' names by their number, a gap left empty: (Lua table, Emacs name,
# header, enumeration, what the table is).
NAMES_BY_NUMBER = (
    ("bye_reasons", "bye-reasons", MESSAGES_HEADER, "ByeReason", "Bye reasons by wire number."),
)

MODIFIER_PREFIX = "keymapModifier"


# What the fingerprint constant is, in both outputs.
FINGERPRINT_WHAT = "The Sync wire fingerprint these tables were written at, sent in every Greet."


def wireFingerprintBytes(text):
    """The 32 bytes `wireFingerprint` holds in Envelope.h, or None."""
    found = re.search(r"\bwireFingerprint\s*\{([^}]*)\}", withoutComments(text))
    if not found:
        return None
    values = [int(token, 0) for token in re.findall(r"0x[0-9a-fA-F]+|\d+", found.group(1))]
    return values if len(values) == 32 and all(0 <= value <= 0xFF for value in values) else None


def scalarValue(texts, header, name):
    """One `SCALARS` number: a constant's value, or an enumeration member's."""
    if "::" in name:
        enumeration, member = name.split("::")
        return dict(cppEnum(texts[header], enumeration)).get(member)
    return headerConstants([texts[header]]).get(name)


def tables(texts):
    """Everything the outputs hold, read from the headers.

    - Parameter texts: {path: text} for every path of `SOURCES`.
    - Returns: a dict of `scalars` {key: value}, `enumerations` {table:
      [(member, value)]}, `names` {table: [name or None]}, `clientOnlyKinds`
      [(member, value)], `connectionKinds` [(member, value)], `modifiers` [(name, value)], `virtualKeys` [(member,
      value)] and `interrupts` [interrupt, quit, suspend].
    - Raises: `Unreadable` naming the first declaration that did not read.
    """
    out = {"scalars": {}, "enumerations": {}, "names": {}}
    for key, _emacs, header, name, _what in SCALARS:
        value = scalarValue(texts, header, name)
        if value is None:
            raise Unreadable("%s in %s" % (name, header))
        out["scalars"][key] = value
    for table, _prefix, header, enumeration, _what in ENUMERATIONS:
        members = cppEnum(texts[header], enumeration)
        if not members:
            raise Unreadable("enum %s in %s" % (enumeration, header))
        out["enumerations"][table] = members
    for table, _emacs, header, enumeration, _what in NAMES_BY_NUMBER:
        members = cppEnum(texts[header], enumeration)
        if not members:
            raise Unreadable("enum %s in %s" % (enumeration, header))
        byNumber = {value: member for member, value in members}
        out["names"][table] = [byNumber.get(number) for number in range(max(byNumber) + 1)]
    kinds = dict(out["enumerations"]["kind"])
    clientOnly = clientToHostKinds(texts[ENVELOPE_SOURCE])
    if not clientOnly or not clientOnly <= set(kinds):
        raise Unreadable("directionOf's client-to-host kinds in %s" % ENVELOPE_SOURCE)
    out["clientOnlyKinds"] = sorted(((member, kinds[member]) for member in clientOnly), key=lambda pair: pair[1])
    scoped = connectionKinds(texts[ENVELOPE_SOURCE])
    if not scoped or not scoped <= set(kinds):
        raise Unreadable("isConnectionKind's kinds in %s" % ENVELOPE_SOURCE)
    out["connectionKinds"] = sorted(((member, kinds[member]) for member in scoped), key=lambda pair: pair[1])
    keymap = headerConstants([texts[KEYMAP_HEADER]])
    out["modifiers"] = sorted(((name[len(MODIFIER_PREFIX):], value) for name, value in keymap.items()
                               if name.startswith(MODIFIER_PREFIX)), key=lambda pair: pair[1])
    if not out["modifiers"]:
        raise Unreadable("the %s* constants in %s" % (MODIFIER_PREFIX, KEYMAP_HEADER))
    out["virtualKeys"] = cppEnum(texts[KEYS_HEADER], "VirtualKey")
    if not out["virtualKeys"]:
        raise Unreadable("enum VirtualKey in %s" % KEYS_HEADER)
    out["fingerprint"] = wireFingerprintBytes(texts[ENVELOPE_HEADER])
    if out["fingerprint"] is None:
        raise Unreadable("wireFingerprint in %s" % ENVELOPE_HEADER)
    out["interrupts"] = interruptDefaults(texts[SCREEN_HEADER])
    if len(out["interrupts"]) != 3:
        raise Unreadable("InterruptCharacters::terminalDefaults in %s" % SCREEN_HEADER)
    return out


def number(value, isHexadecimal, lisp):
    """A number as an output writes it."""
    if not isHexadecimal:
        return str(value)
    return ("#x%02X" if lisp else "0x%02X") % value


def wrapped(items, indent, width=96):
    """`items` joined by spaces into lines no wider than `width`."""
    lines = []
    line = ""
    for item in items:
        if line and len(indent) + len(line) + 1 + len(item) > width:
            lines.append(indent + line)
            line = item
        else:
            line = (line + " " + item) if line else item
    if line:
        lines.append(indent + line)
    return "\n".join(lines)


def sourceList(comment):
    """The headers the tables come from, one per comment line."""
    return "\n".join("%s   %s" % (comment, path.replace(os.sep, "/")) for path in SOURCES)


def emacsText(texts):
    """The Emacs Lisp file, whole."""
    read_ = tables(texts)
    out = [
        ";;; ubiquitty-sync-wire.el --- Wire numbers for ubiquitty-sync  -*- lexical-binding: t; -*-",
        "",
        ";; Copyright (C) 2026 Stanislav Yaglo",
        ";; SPDX-License-Identifier: MIT",
        "",
        ";;; Commentary:",
        "",
        ";; The numbers `ubiquitty-sync' shares with the host, as the host's headers",
        ";; declare them.  Written by %s from:" % GENERATOR,
        ";;",
        sourceList(";;"),
        ";;",
        ";; Do not edit: change the header and run the script.",
        ";; Scripts/check-wire-tables.py fails when this file is not what the headers",
        ";; produce.",
        "",
        ";;; Code:",
        "",
    ]
    for key, emacs, _header, name, what in SCALARS:
        out.append('(defconst ubiquitty-sync--%s %s\n  "%s")'
                   % (emacs, number(read_["scalars"][key], key in HEXADECIMAL, True), what))
    out.append('(defconst ubiquitty-sync--wire-fingerprint\n  (unibyte-string %s)\n  "%s")'
               % (wrapped([str(value) for value in read_["fingerprint"]], "   ").lstrip(), FINGERPRINT_WHAT))
    for table, prefix, _header, enumeration, what in ENUMERATIONS:
        out.append("")
        out.append(";; %s." % enumeration)
        for member, value in read_["enumerations"][table]:
            out.append('(defconst ubiquitty-sync--%s-%s %s "%s: %s.")'
                       % (prefix, kebab(member), number(value, table in HEXADECIMAL, True), what, member))
    for table, emacs, _header, _enumeration, what in NAMES_BY_NUMBER:
        out.append("")
        names = ['"%s"' % name if name else "nil" for name in read_["names"][table]]
        out.append("(defconst ubiquitty-sync--%s\n%s\n  \"%s\")"
                   % (emacs, "  [" + wrapped(names, "   ").lstrip() + "]", what))
    out.append("")
    out.append("(defconst ubiquitty-sync--client-only-kinds\n%s\n  \"%s\")"
               % ("  '(" + wrapped([str(value) for _member, value in read_["clientOnlyKinds"]], "    ").lstrip() + ")",
                  "The kinds only a client sends (Envelope.cpp's directionOf):\n"
                  + ", ".join(member for member, _value in read_["clientOnlyKinds"][:8]) + ",\n"
                  + ", ".join(member for member, _value in read_["clientOnlyKinds"][8:]) + "."))
    out.append("")
    out.append(";; Key encoder modifier bits.")
    for name, value in read_["modifiers"]:
        out.append('(defconst ubiquitty-sync--modifier-%s %d "Key encoder modifier bit: %s.")' % (kebab(name), value, name))
    out.append("")
    keys = ["(%s . %d)" % (kebab(member), value) for member, value in read_["virtualKeys"]]
    out.append("(defconst ubiquitty-sync--virtual-key-numbers\n%s\n  \"%s\")"
               % ("  '(" + wrapped(keys, "    ").lstrip() + ")", "`Keys::VirtualKey' numbers by name."))
    out.append("")
    out.append("(defconst ubiquitty-sync--default-interrupt-characters '(%s)\n  \"%s\")"
               % (" ".join(str(value) for value in read_["interrupts"]),
                  "The interrupt, quit and suspend characters of a pane nobody has described."))
    out.append("")
    out.append("(defconst ubiquitty-sync--known-kinds '(%s)\n  \"Current canonical envelope kinds.\")"
               % " ".join(str(value) for _member, value in read_["enumerations"]["kind"]))
    out.append("(defconst ubiquitty-sync--connection-kinds '(%s)\n  \"Kinds requiring canonical channel zero.\")"
               % " ".join(str(value) for _member, value in read_["connectionKinds"]))
    out += ["", "(provide 'ubiquitty-sync-wire)", "", ";;; ubiquitty-sync-wire.el ends here", ""]
    return "\n".join(out)


# Words Lua keeps for itself, which a table key has to be quoted to use.
LUA_KEYWORDS = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local",
                "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"}


def luaKey(key):
    """A Lua table key: bare, or quoted when it is one of Lua's own words."""
    return '["%s"]' % key if key in LUA_KEYWORDS else key


def luaTable(name, pairs, isHexadecimal):
    """A Lua table of `key = number` entries, one a line."""
    lines = ["M.%s = {" % name]
    lines += ["    %s = %s," % (luaKey(key), number(value, isHexadecimal, False)) for key, value in pairs]
    lines.append("}")
    return "\n".join(lines)


def luaText(texts):
    """The Lua file, whole."""
    read_ = tables(texts)
    out = [
        "-- Copyright (C) 2026 Stanislav Yaglo",
        "-- SPDX-License-Identifier: Apache-2.0",
        "",
        "-- The numbers ubiquitty_sync shares with the host, as the host's headers",
        "-- declare them. Written by %s from:" % GENERATOR,
        "--",
        sourceList("--"),
        "--",
        "-- Do not edit: change the header and run the script.",
        "-- Scripts/check-wire-tables.py fails when this file is not what the headers",
        "-- produce.",
        "",
        "local M = {}",
        "",
    ]
    for key, _emacs, _header, _name, what in SCALARS:
        out.append("-- %s" % what)
        out.append("M.%s = %s" % (key, number(read_["scalars"][key], key in HEXADECIMAL, False)))
    out.append("-- %s" % FINGERPRINT_WHAT)
    out.append('M.wire_fingerprint = "%s"' % "".join("\\%03d" % value for value in read_["fingerprint"]))
    for table, _prefix, _header, enumeration, _what in ENUMERATIONS:
        out.append("")
        out.append("-- %s." % enumeration)
        out.append(luaTable(table, [(snake(member), value) for member, value in read_["enumerations"][table]],
                            table in HEXADECIMAL))
    for table, _emacs, _header, _enumeration, what in NAMES_BY_NUMBER:
        out.append("")
        out.append("-- %s" % what.replace("nil where", "absent where"))
        out.append("M.%s = {" % table)
        out += ['    [%d] = "%s",' % (index, name) for index, name in enumerate(read_["names"][table]) if name]
        out.append("}")
    out.append("")
    out.append("-- The kinds only a client sends (Envelope.cpp's directionOf).")
    out.append("M.client_only_kind = {")
    out += ["    [%d] = true, -- %s" % (value, member) for member, value in read_["clientOnlyKinds"]]
    out.append("}")
    out.append("")
    out.append("-- Key encoder modifier bits.")
    out.append(luaTable("modifier", [(snake(name), value) for name, value in read_["modifiers"]], False))
    out.append("")
    out.append("-- Keys::VirtualKey numbers by name.")
    out.append(luaTable("virtual_key", [(snake(member), value) for member, value in read_["virtualKeys"]], False))
    out.append("")
    out.append("-- The interrupt, quit and suspend characters of a pane nobody has described.")
    out.append("M.default_interrupt_characters = { interrupt = 0x%02x, quit = 0x%02x, suspend = 0x%02x }" % tuple(read_["interrupts"]))
    out.append("")
    out.append("-- Kinds requiring canonical channel zero.")
    out.append("M.connection_kind = { %s }" % ", ".join("[%d] = true" % value for _member, value in read_["connectionKinds"]))
    out += ["", "return M", ""]
    return "\n".join(out)


# What each output is written by, for the scripts that write and compare them.
OUTPUTS = ((EMACS_OUTPUT, emacsText), (NEOVIM_OUTPUT, luaText))
