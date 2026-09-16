#include "Error.h"

namespace ptacxx {

Error::~Error() = default;
AssertionViolation::~AssertionViolation() = default;
ConfigError::~ConfigError() = default;
InputBitcodeError::~InputBitcodeError() = default;
SyntaxError::~SyntaxError() = default;
VidNotFound::~VidNotFound() = default;
NoteRelated::~NoteRelated() = default;
SemanticError::~SemanticError() = default;
FileSystemError::~FileSystemError() = default;
AnalyzerError::~AnalyzerError() = default;
TimespecInvalid::~TimespecInvalid() = default;

} // namespace ptacxx
