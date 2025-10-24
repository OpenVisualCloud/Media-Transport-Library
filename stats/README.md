# Repository Statistics

This directory contains historical repository traffic statistics collected automatically.

## File Structure

### Consolidated Statistics
- `clones.json` - All unique daily clone statistics merged from historical collections
- `views.json` - All unique daily view statistics merged from historical collections

Format (same as GitHub API):
```json
{
  "count": 1234,
  "uniques": 567,
  "clones": [
    {
      "timestamp": "2025-10-03T00:00:00Z",
      "count": 4,
      "uniques": 1
    }
  ]
}
```

### Historical Dated Files (Source Data)
Historical dated files follow the pattern:
- `clones_YYYYMMDD.json` - Clone statistics snapshot from specific collection date
- `views_YYYYMMDD.json` - View statistics snapshot from specific collection date

These files contain 15-day rolling window data as provided by GitHub's API on that date.
