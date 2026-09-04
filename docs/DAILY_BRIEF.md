# Daily Feature

## Goal

Add two focused features to standard CrossPoint:

- Calendar
- Bible

CrossPoint must remain fully usable as an e-reader.

## Manual refresh

When the device starts or a Daily screen opens:

1. Load the last cached verse, calendar, and focus JSON immediately.
2. Display the requested screen without enabling Wi-Fi or waiting for the network.

The user starts refresh manually with the **Sync** button on the homepage. Sync
connects to the first available saved Wi-Fi network, shows a loading screen,
then fetches `/verse`, `/calendar`, and `/focus`. Validate and cache each
response independently, and keep the previous cache when a request fails.

## Server configuration

Default base URL:

```text
https://my-daily.allan.ch/server
```

The default must exist as a clearly named constant in code.

Add a **Daily** section to CrossPoint Settings containing:

- Server URL
- Bearer token
- Manual refresh action
- Reload all Daily API endpoints action
- Last successful refresh time
- Connection or refresh status
- Calendar unlock combination setup: record a four-button combination using
  logical device buttons, in order

The saved Settings value overrides the compiled default.

Endpoints:

```text
GET  {baseUrl}/verse
GET  {baseUrl}/calendar
GET  {baseUrl}/focus
POST {baseUrl}/tasks/{taskId}
```

Use Bearer authentication for every request to the exact
`my-daily.allan.ch` host. Never forward that header across a redirect to a
different host. The token is a local secret and must not be committed to the
repository. The three GET requests
are independent: `/verse` returns only the current daily Bible verse as JSON,
while `/calendar` returns the complete calendar and task JSON for the next
five months, not only today's events, and `/focus` returns the timetable that
controls which calendars are visible at each time.

Opening Calendar requires entering the recorded four-button combination. Ask
for it at most once per local calendar day; after a successful entry, Calendar
remains unlocked until the next local calendar day. Failed entries do not
unlock it. Store the combination in the existing Settings mechanism and never
write it to logs or include it in network requests.

Task update body:

```json
{
  "completed": true
}
```

Failed task updates must be queued locally and retried during the next successful Wi-Fi connection.

## Homepage

Add these buttons immediately before **Browse Files**:

1. Calendar
2. Bible
3. Browse Files
   ...

## Calendar

Calendar data comes from `/calendar` and includes the full five-month range
returned by the endpoint. A payload may contain multiple named calendars,
such as `work` and `private`; events and tasks belong to exactly one calendar.
Cache the validated payload for offline use.

The Calendar and Daily Visualizer views must group or sort today's items by
calendar and show the calendar name for each group.

### Focus timetable

Focus data comes from `/focus`. It defines time windows that select which
calendar IDs are visible; tasks are included because they belong to calendars.
Rules may be limited to particular weekdays and may cross midnight. When
multiple rules match, the most specific matching rule wins; otherwise all
calendars are visible. An empty selected-calendar list hides calendar items
and tasks for that time window.

On every load of Calendar or the Daily Visualizer, evaluate the cached focus
timetable against the current local date and time. Do not continuously reload
the API while a screen is open. A successful refresh of `/focus` takes effect
on the next screen load.

It supports two views:

### Week view

- Show one week of events and tasks.
- Up and Down move to the previous or next week.
- All-day items appear separately from timed items.
- Display tags and locations where available.

### Day view

- Show one day in greater detail.
- Up and Down move to the previous or next day.
- Separate all-day events, timed events and tasks.
- Tasks are selectable checkboxes.

Left and Right switch between Week and Day views.

Save the most recently used view and restore it the next time Calendar opens.

### Calendar item types

Events may contain:

- ID
- Title
- Date
- Start time
- End time
- All-day status
- Location
- Tags

Tasks may contain:

- ID
- Title
- Date
- Completed status
- Location
- Tags

Every event and task must include a `calendarId` identifying one of the named
calendars in the payload.

Selecting a task toggles its completed state immediately on-screen.

The API update may happen immediately or remain queued until Wi-Fi is available.

## Bible

The verse of the day is supplied by the `/verse` API. Use the returned
`verse.text` directly for the daily verse text, `verse.reference` for its
reference, and `verse.translation` for the displayed translation. Cache the
validated API response so the verse of the day remains available offline.

The verse of the day must not depend on a matching local Bible version or
chapter file. Local Bible text and notes on the SD card are used only by the
Bible reader for browsing books and chapters.

### Bible home

Approximately 70% of the screen displays:

- Verse of the day
- Verse reference
- Translation returned by the verse API

The lower part of the screen contains a Bible-book selector.

Use Left and Right to select a book and Confirm to open it.

When the daily reference can be resolved against the selected manifest, start
with that book selected. Opening it starts with the referenced chapter selected,
and opening the chapter starts on the first referenced verse. A small segmented
gauge shows `memorisation.level` relative to `memorisation.scale`. Daily text
that does not fit the available home-screen area is clipped rather than growing
the layout.

### Bible reader

After selecting a book:

1. Show its available chapters.
2. Select a chapter using the device buttons.
3. Open the chapter in a readable text view.
4. Allow navigation to the previous and next chapters.
5. Allow returning to Bible home.
6. Allow switching Bible versions.

Save the most recently used Bible version and restore it the next time Bible
opens. If that version is no longer available, fall back to an available
version using the normal selection rules.

Initial versions:

- English: TPT
- French: LS21

Bible versions must be detected from the files available on the SD card rather than hardcoded to only these two versions. If a book is missing from the version, switch to next available version in same Language.

If the API translation abbreviation does not match a detected local version,
treat it as a custom version and show the API text for the referenced
chapter or excerpt. If the API translation name ends in ` Modified`, remove
that suffix from both the name and abbreviation when resolving the local source
version. Continue to display the custom API text for the daily excerpt, but use
the resolved source version for adjacent chapters and normal Bible navigation.

When `verse.notes.personal` contains text, append it as a footnote after the
custom API excerpt. Local `<CHAPTER>.notes.json` notes retain their existing
interactive note behavior.

## Bible storage format

Use UTF-8 plain-text chapter files with a small manifest for each version.

Suggested layout:

```text
/bibles/
  TPT/
    manifest.json
    GEN/
      001.txt
      002.txt
    PSA/
      001.txt
      002.txt
    REV/
      020.notes.json
  LS21/
    manifest.json
    GEN/
      001.txt
    PSA/
      001.txt
```

Each chapter file contains one verse per line:

```text
1<TAB>Verse text
2<TAB>Verse text
3<TAB>Verse text
```

Example:

```text
1	Blessed is the one...
2	But whose delight is...
```

The manifest contains:

- Version ID
- Display name
- Language
- Book IDs
- Book names
- Book order
- Chapter counts

Optional chapter notes are stored beside the chapter text using this file
pattern:

```text
<BOOK>/<CHAPTER>.notes.json
```

For example, `REV/020.notes.json` contains `book_id`, `chapter`, and a
`notes` array. Each note's `verse` field links it to a verse and its `text`
field contains the note text. When notes exist for a verse, the Bible reader
must make them available with that verse. Missing or malformed notes files are
treated as having no notes and must not prevent chapter text from loading.

Do not bundle copyrighted Bible text in the public repository unless redistribution is permitted. Bible content will be supplied separately.

## Verse and calendar JSON

`/verse` JSON structure:

```json
{
  "success": true,
  "date": "2026-08-02",
  "verse": {
    "id": 1,
    "reference": "Genèse 2:5",
    "translation": {
      "abbreviation": "S21",
      "name": "La Bible Segond 21",
      "language": "fr"
    },
    "text": "5 Lorsque l'Eternel Dieu fit la terre et le ciel, il n'y avait encore aucun arbuste des champs sur la terre et aucune herbe des champs ne poussait encore, car l'Eternel Dieu n'avait pas fait pleuvoir sur la terre et il n'y avait pas d'homme pour cultiver le sol.",
    "memorisation": {
      "level": 1,
      "scale": 3
    },
    "notes": {
      "personal": null,
      "source": [
        {
          "text": "2.5 L'Eternel Dieu: dans cette version, l'Eternel traduit le tétragramme Yhvh que les Juifs, par respect, ont depuis longtemps cessé de prononcer. Ce tétragramme est à rapprocher de la racine hébraïque du verbe être (voir aussi Exode 3.14-15). Dieu traduit le plus souvent l'hébreu 'elohim (comme ici) ou 'el.",
          "verse": "5"
        }
      ]
    }
  }
}
```

A valid daily verse response requires `success` to be `true` and must contain
`date`, a parseable `verse.reference`, `verse.translation.abbreviation`,
`verse.translation.name`, `verse.translation.language`, and non-empty
`verse.text`. References use the form `<book> <chapter>:<verse>` or
`<book> <chapter>:<first>-<last>`. The Daily screens display the cached `verse.text` without
resolving or replacing it from local Bible files. Memorisation data and notes
are cached when present. Oversized display text and personal notes are safely
truncated to their fixed firmware buffers.

`/calendar` JSON structure:

```json
{
  "schemaVersion": 1,
  "date": "2026-08-01",
  "generatedAt": "2026-08-01T06:00:00+02:00",
  "range": { "from": "2026-08-01", "to": "2026-12-31" },
  "calendars": [
    {
      "id": "work",
      "name": "Work",
      "events": [
        {
          "id": "work-101",
          "calendarId": "work",
          "title": "Project review",
          "date": "2026-08-03",
          "startTime": "09:00",
          "endTime": "10:00",
          "allDay": false
        }
      ],
      "tasks": []
    },
    {
      "id": "private",
      "name": "Private",
      "events": [],
      "tasks": [
        {
          "id": "private-201",
          "calendarId": "private",
          "title": "Buy groceries",
          "date": "2026-08-03",
          "completed": false
        }
      ]
    }
  ]
}
```

`/focus` JSON structure:

```json
{
  "schemaVersion": 1,
  "generatedAt": "2026-08-01T06:00:00+02:00",
  "rules": [
    {
      "id": "monday-morning",
      "weekdays": ["monday"],
      "from": "08:00",
      "to": "12:00",
      "calendarIds": ["mystik", "holidays"]
    },
    {
      "id": "default",
      "weekdays": ["*"],
      "from": "00:00",
      "to": "24:00",
      "calendarIds": ["work", "private", "mystik", "holidays"]
    }
  ]
}
```

The calendar `range` must cover the next five months according to the
server's local date. Reject unsupported calendar or focus schema versions or
an insufficient calendar range, and retain the previous valid cache for that
API. Reject focus rules that reference unknown calendar IDs. Verse, calendar,
and focus caches are independent: failure of one request must not discard
valid caches from the others.

## Power-off screen

When the device is turned off, render a Daily Visualizer before entering the existing CrossPoint shutdown flow.

The retained image must contain:

- Current date
- Verse of the day
- Verse reference
- Today’s all-day events
- Today’s timed events
- Today’s incomplete tasks

Group these items by calendar and apply the currently matching `/focus`
rule before rendering them. Use cached data only. Do not delay shutdown while
waiting for a network request.

If no valid cached daily verse response with non-empty `verse.text` exists,
render a white screen with this text centered on the display instead of the
normal CrossPoint sleep screen:

```text
edwin@allan.ch
+41795846697
```

The normal sleep-screen renderer must not overwrite the Daily Visualizer after it has been drawn.

## Development order

1. Add the homepage buttons and screen navigation.
2. Build the Bible reader using the Bible files already on the SD card.
3. Add the verse cache.
4. Build the Bible home screen.
5. Build the Calendar views using my-daily.allan.ch api.
6. Add focus filtering.
7. Add Calendar unlocking.
8. Add Daily settings.
9. Add API loading and caching.
10. Add manual homepage sync.
11. Add calendar task updates and offline queueing.
12. Add the Daily Visualizer power-off screen.
13. Validate the complete feature in the simulator.
14. Validate the complete feature on the physical Xteink X3.
