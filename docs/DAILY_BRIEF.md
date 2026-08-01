# Daily Feature

## Goal

Add two focused features to standard CrossPoint:

- Calendar
- Bible

CrossPoint must remain fully usable as an e-reader.

## Boot refresh

When the device starts:

1. Load the last cached daily JSON immediately.
2. Display the normal homepage without waiting for the network.
3. If Wi-Fi is configured, fetch updated data in a background task.
4. Validate and cache the new response.
5. Refresh any visible Daily screen when the fetch completes.
6. Keep the previous cache when the request fails.

This is a startup refresh, not continuous background synchronization.

## Server configuration

Default base URL:

```text
https://example.com/crosspoint-daily
```

The default must exist as a clearly named constant in code.

Add a **Daily** section to CrossPoint Settings containing:

- Server URL
- Refresh on startup toggle
- Manual refresh action
- Last successful refresh time
- Connection or refresh status

The saved Settings value overrides the compiled default.

Endpoints:

```text
GET  {baseUrl}/daily.json
POST {baseUrl}/tasks/{taskId}
```

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

Calendar data comes from `daily.json`.

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

Selecting a task toggles its completed state immediately on-screen.

The API update may happen immediately or remain queued until Wi-Fi is available.

## Bible

Bible text is stored locally on the SD card and is not downloaded in `daily.json`.

The Daily JSON only identifies the verse of the day:

```json
{
  "version": "TPT",
  "book": "Psalms",
  "chapter": 118,
  "verse": 24,
  "reference": "Psalm 118:24"
}
```

The displayed verse text is resolved from the local Bible files.

### Bible home

Approximately 70% of the screen displays:

- Verse of the day
- Verse reference
- Active Bible version

The lower part of the screen contains a Bible-book selector.

Use Left and Right to select a book and Confirm to open it.

### Bible reader

After selecting a book:

1. Show its available chapters.
2. Select a chapter using the device buttons.
3. Open the chapter in a readable text view.
4. Allow navigation to the previous and next chapters.
5. Allow returning to Bible home.
6. Allow switching Bible versions.

Initial versions:

- English: TPT
- French: LS21

Bible versions must be detected from the files available on the SD card rather than hardcoded to only these two versions. If a book is missing from the version, switch to next available version in same Language.

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

Do not bundle copyrighted Bible text in the public repository unless redistribution is permitted. Bible content will be supplied separately.

## Daily JSON

Proposed structure:

```json
{
  "schemaVersion": 1,
  "date": "2026-08-01",
  "generatedAt": "2026-08-01T06:00:00+02:00",
  "verseOfDay": {
    "version": "TPT",
    "book": "Psalms",
    "chapter": 118,
    "verse": 24,
    "reference": "Psalm 118:24"
  },
  "events": [],
  "tasks": []
}
```

Reject unsupported schema versions and retain the previous valid cache.

## Power-off screen

When the device is turned off, render a Daily Visualizer before entering the existing CrossPoint shutdown flow.

The retained image must contain:

- Current date
- Verse of the day
- Verse reference
- Today’s all-day events
- Today’s timed events
- Today’s incomplete tasks

Use cached data only. Do not delay shutdown while waiting for a network request.

If no valid daily cache or local verse text exists, fall back to the normal CrossPoint sleep screen.

The normal sleep-screen renderer must not overwrite the Daily Visualizer after it has been drawn.

## Development order

1. Add static Calendar and Bible screens in the simulator.
2. Add homepage buttons and navigation.
3. Add local Bible fixture files.
4. Add JSON fixture loading.
5. Add Settings → Daily.
6. Add background startup refresh.
7. Add calendar task updates and offline queueing.
8. Add the Daily Visualizer sleep screen.
9. Validate the complete feature on the physical Xteink X3.

Scheduled automatic wake is not part of this feature.
