// json_util.hpp: minimal string escaping for hand-built JSON output.
// Not a full JSON library - these tools only ever emit a small, fixed shape
// of output, so a full parser/serializer would be more dependency than value.
#pragma once
#include <string>
#include <sstream>

inline std::string json_escape(const std::string &s)
{
	std::ostringstream out;
	for (unsigned char c : s)
	{
		switch (c)
		{
		case '"': out << "\\\""; break;
		case '\\': out << "\\\\"; break;
		case '\n': out << "\\n"; break;
		case '\r': out << "\\r"; break;
		case '\t': out << "\\t"; break;
		default:
			if (c < 0x20)
			{
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				out << buf;
			}
			else
			{
				out << c;
			}
		}
	}
	return out.str();
}
