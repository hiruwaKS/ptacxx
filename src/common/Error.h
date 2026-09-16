#pragma once

#include <exception>
#include <string>
#include <utility>

namespace ptacxx {

/// Base class of all ptacxx exceptions.
///
/// Every error carries a stable, project-unique name that already encodes its
/// category (lowercase-category-kebab-detail) and is registered in
/// docs/ErrorUniqueName.csv, so it can be grepped to locate the throw site.
/// what() is formatted as "<unique-name> <message>".
class Error : public std::exception {
public:
  Error(const char *uniqueName, std::string msg)
      : _message(std::string(uniqueName) + " " + std::move(msg)) {}

  ~Error() override;
  const char *what() const noexcept override { return _message.c_str(); }

private:
  std::string _message;
};

#define PTACXX_ERROR(Name)                                                     \
  class Name : public Error {                                                  \
  public:                                                                      \
    Name(const char *uniqueName, std::string msg)                              \
        : Error(uniqueName, std::move(msg)) {}                                 \
    ~Name() override;                                                          \
  };

/// An internal invariant was violated (a bug, should never happen).
PTACXX_ERROR(AssertionViolation)
/// Command line / configuration misuse or missing configuration.
PTACXX_ERROR(ConfigError)
/// The input IR module could not be loaded or is invalid.
PTACXX_ERROR(InputBitcodeError)
/// A query is malformed.
PTACXX_ERROR(SyntaxError)
/// A vid referenced by a query does not exist (or is ambiguous).
PTACXX_ERROR(VidNotFound)
/// A note / theorem / proposition referenced by a query is missing.
PTACXX_ERROR(NoteRelated)
/// A query is well-formed but semantically invalid.
PTACXX_ERROR(SemanticError)
/// A file system operation failed.
PTACXX_ERROR(FileSystemError)
/// The underlying analyzer failed.
PTACXX_ERROR(AnalyzerError)
/// The current time could not be read.
PTACXX_ERROR(TimespecInvalid)

#undef PTACXX_ERROR

} // namespace ptacxx

#define ASSERT(cond, uniqueName, msg)                                          \
  do {                                                                         \
    if (!(cond))                                                               \
      throw ::ptacxx::AssertionViolation(uniqueName, std::string(msg));        \
  } while (false)
