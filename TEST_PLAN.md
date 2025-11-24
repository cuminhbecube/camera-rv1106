# TEST PLAN - v2.1 Simplified

## Strategy: INCREMENTAL TESTING

### Phase 1: Minimal Working Version ✅
**Keep Only**:
- Status API (working)
- FPS API (working) 
- Logs API

**Remove**:
- Bitrate API (crashes)
- Resolution API (crashes)
- Control API (may crash)

### Phase 2: Build & Deploy
1. Compile simplified version
2. Test on board thoroughly
3. Build firmware with working features only
4. Flash and verify

### Phase 3: Add Features Back (One by One)
1. Add Resolution API → Test → If crashes, debug
2. Add Bitrate API → Test → If crashes, debug  
3. Add Control API → Test

## Current Status

**Working APIs** (v2.1):
- ✅ GET /api/status
- ✅ GET /api/fps?fps=<1-30>
- ✅ GET /api/logs

**Build Firmware Now?**
YES - Build with 3 working APIs, better than nothing!

## Action Items

1. Comment out crashing APIs in source
2. Recompile
3. Test all working APIs
4. Build firmware
5. Document limitations

---

**Decision**: Proceed with WORKING FEATURES ONLY for v2.1
**Note**: Bitrate/Resolution can be added in v2.2 after debugging
