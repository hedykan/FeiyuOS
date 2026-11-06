#pragma once

namespace vat::web {

// On-device admin page and JSON API (port 80):
//   GET  /                     configuration page
//   GET  /api/status           device + worker + screen state
//   GET  /api/file?path=&tail= read a /vat file (tail bytes optional)
//   POST /api/file?path=       write body as a /vat text file
//   POST /api/upload?path=     multipart upload (binary, e.g. PCM or key)
//   POST /api/delete?path=     delete a /vat file or directory
//   GET  /api/ls?path=         list a /vat directory
//   POST /api/job?kind=...     submit a worker job, returns {"id":N}
//   GET  /api/job?id=N         job record
//   POST /api/action?name=...  approve|reject|cancel|page_*|toggle|help
//   POST /api/secret           submit a requested secret (text/plain body)
//   GET  /api/screenshot       current screen as BMP
//   POST /api/reload           reload configuration
//   POST /api/reboot
void begin();
void loop();

}  // namespace vat::web
