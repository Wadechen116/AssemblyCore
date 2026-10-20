#pragma once
// The assembler counts columns in UTF-8 bytes; Qt counts UTF-16 units. These convert between them (they differ only on lines holding
// non-ASCII text, e.g. a Chinese comment).
#include <QByteArray>
#include <QString>

namespace acide {

inline int charFromByte(const QString& line, int byteCol) {
    if (byteCol <= 0) return 0;
    QByteArray u = line.toUtf8();
    if (byteCol >= u.size()) return line.size();
    return QString::fromUtf8(u.constData(), byteCol).size();
}
inline int byteFromChar(const QString& line, int charCol) {
    if (charCol <= 0) return 0;
    return line.left(charCol).toUtf8().size();
}
inline std::string toStd(const QString& s) { QByteArray b = s.toUtf8(); return std::string(b.constData(), (size_t)b.size()); }
inline QString fromStd(const std::string& s) { return QString::fromUtf8(s.data(), (int)s.size()); }

}  // namespace acide
