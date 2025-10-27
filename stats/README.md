# Repository Statistics

This directory contains historical repository traffic statistics collected automatically.

## File Structure

### Consolidated Statistics (Encrypted)
- `clones.json.enc` - Encrypted consolidated clone statistics with all unique daily entries
- `views.json.enc` - Encrypted consolidated view statistics with all unique daily entries

**Note:** Files are encrypted using AES-256-CBC encryption for security. The workflow automatically decrypts, updates, and re-encrypts them.

#### Decrypted Format
When decrypted, files follow the GitHub API format:
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

#### To Decrypt Locally
```bash
openssl enc -aes-256-cbc -d -pbkdf2 -in clones.json.enc -out clones.json -k "YOUR_KEY"
openssl enc -aes-256-cbc -d -pbkdf2 -in views.json.enc -out views.json -k "YOUR_KEY"
```

## Data Collection

Stats are collected daily via GitHub Actions workflow and automatically merged into the encrypted consolidated files. The workflow:
1. Decrypts existing files
2. Fetches new data from GitHub API (14-day rolling window)
3. Merges unique daily entries
4. Re-encrypts and commits updated files
