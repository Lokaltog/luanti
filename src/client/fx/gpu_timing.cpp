// Luanti "fx" fork: GPU time per render step (see gpu_timing.h)
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "gpu_timing.h"

#include "log.h"
#include <mt_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

namespace fx::gpu
{

namespace
{

using GLuint = unsigned int;
using GLuint64 = khronos_uint64_t;

constexpr int FRAMES = 4; // in flight before a frame's results are read

struct Span
{
	const char *name;
	int parent;     // index into the frame's spans, -1 for top level
	GLuint q0 = 0, q1 = 0;
};

struct Frame
{
	std::vector<Span> spans;
	GLuint begin = 0, end = 0;
	bool used = false;
};

struct Row
{
	std::string path; // parent path + "/" + name, for aggregation
	std::string name;
	int depth = 0;
	double ms = 0.0;   // smoothed
	int order = 0;     // first appearance, for display order
	int seen = 0;      // frames since last present
};

struct State
{
	bool on = false;
	bool supported = true;
	bool checked = false;
	Frame frames[FRAMES];
	int cur = 0;
	bool in_frame = false;
	std::vector<int> stack;
	std::vector<GLuint> pool;
	std::map<std::string, Row> rows;
	int next_order = 0;
	double total_ms = 0.0;
	u64 frames_read = 0;
};

State &st()
{
	static State s;
	return s;
}

GLuint takeQuery()
{
	auto &s = st();
	if (s.pool.empty()) {
		GLuint q[16];
		GL.GenQueries(16, q);
		s.pool.insert(s.pool.end(), q, q + 16);
	}
	GLuint q = s.pool.back();
	s.pool.pop_back();
	return q;
}

void giveBack(Frame &f)
{
	auto &s = st();
	for (auto &sp : f.spans) {
		s.pool.push_back(sp.q0);
		if (sp.q1)
			s.pool.push_back(sp.q1);
	}
	if (f.begin)
		s.pool.push_back(f.begin);
	if (f.end)
		s.pool.push_back(f.end);
	f.spans.clear();
	f.begin = f.end = 0;
	f.used = false;
}

GLuint64 stamp(GLuint q)
{
	GLuint64 v = 0;
	GL.GetQueryObjectui64v(q, GL.QUERY_RESULT, &v);
	return v;
}

void smooth(double &acc, double v, bool first)
{
	acc = first ? v : acc + (v - acc) * 0.1;
}

// read a finished frame into the rows
void collect(Frame &f)
{
	auto &s = st();
	if (!f.used || !f.end)
		return;
	GLuint ready = 0;
	GL.GetQueryObjectuiv(f.end, GL.QUERY_RESULT_AVAILABLE, &ready);
	if (!ready)
		return; // try again next frame (the ring slot is reused then anyway)
	const bool first = s.frames_read == 0;
	s.frames_read++;
	// busy time: the top-level spans (the gaps between them are the CPU)
	double busy = 0.0;
	for (const Span &sp : f.spans)
		if (sp.parent < 0)
			busy += (double)(stamp(sp.q1) - stamp(sp.q0)) * 1e-6;
	smooth(s.total_ms, busy, first);

	std::vector<std::string> paths(f.spans.size());
	std::map<std::string, double> sums;
	std::map<std::string, std::pair<std::string, int>> meta;
	std::vector<std::string> first_seen; // paths in issue order
	for (size_t i = 0; i < f.spans.size(); i++) {
		const Span &sp = f.spans[i];
		paths[i] = (sp.parent >= 0 ? paths[sp.parent] + "/" : std::string()) + sp.name;
		const double ms = (double)(stamp(sp.q1) - stamp(sp.q0)) * 1e-6;
		if (!sums.count(paths[i]))
			first_seen.push_back(paths[i]);
		sums[paths[i]] += ms;
		int depth = 0;
		for (int p = sp.parent; p >= 0; p = f.spans[p].parent)
			depth++;
		meta[paths[i]] = {sp.name, depth};
	}
	for (auto &r : s.rows)
		r.second.seen++;
	for (const std::string &path : first_seen) {
		const double ms = sums[path];
		auto it = s.rows.find(path);
		const bool fresh = it == s.rows.end();
		Row &r = s.rows[path];
		if (fresh) {
			r.path = path;
			r.name = meta[path].first;
			r.depth = meta[path].second;
			r.order = s.next_order++;
		}
		smooth(r.ms, ms, fresh);
		r.seen = 0;
	}
	// forget rows that stopped appearing (a pass turned off)
	for (auto it = s.rows.begin(); it != s.rows.end();)
		it = it->second.seen > 120 ? s.rows.erase(it) : std::next(it);
}

bool checkSupport()
{
	auto &s = st();
	if (s.checked)
		return s.supported;
	s.checked = true;
	s.supported = GL.QueryCounter && GL.GenQueries && GL.GetQueryObjectui64v;
	if (!s.supported)
		warningstream << "fx: GPU timing needs timer queries (GL 3.3 / ARB_timer_query)" << std::endl;
	return s.supported;
}

} // namespace

void setEnabled(bool on)
{
	auto &s = st();
	if (on == s.on)
		return;
	s.on = on && checkSupport();
	if (!s.on) {
		for (auto &f : s.frames)
			giveBack(f);
		s.rows.clear();
		s.next_order = 0;
		s.frames_read = 0;
		s.stack.clear();
		s.in_frame = false;
	}
}

bool enabled()
{
	return st().on;
}

void frameBegin()
{
	auto &s = st();
	if (!s.on || s.in_frame)
		return;
	s.cur = (s.cur + 1) % FRAMES;
	Frame &f = s.frames[s.cur];
	// the oldest frame in the ring: its results are (almost always) ready
	collect(f);
	giveBack(f);
	f.used = true;
	f.begin = takeQuery();
	GL.QueryCounter(f.begin, GL.TIMESTAMP);
	s.stack.clear();
	s.in_frame = true;
}

void frameEnd()
{
	auto &s = st();
	if (!s.on || !s.in_frame)
		return;
	Frame &f = s.frames[s.cur];
	for (auto &sp : f.spans)
		if (!sp.q1) {
			sp.q1 = takeQuery();
			GL.QueryCounter(sp.q1, GL.TIMESTAMP);
		}
	f.end = takeQuery();
	GL.QueryCounter(f.end, GL.TIMESTAMP);
	s.in_frame = false;
}

Scope::Scope(const char *name)
{
	auto &s = st();
	if (!s.on || !s.in_frame)
		return;
	Frame &f = s.frames[s.cur];
	Span sp;
	sp.name = name;
	sp.parent = s.stack.empty() ? -1 : s.stack.back();
	sp.q0 = takeQuery();
	GL.QueryCounter(sp.q0, GL.TIMESTAMP);
	m_index = (int)f.spans.size();
	f.spans.push_back(sp);
	s.stack.push_back(m_index);
}

Scope::Scope(const std::string &name) : Scope(intern(name)) {}

Scope::~Scope()
{
	auto &s = st();
	if (m_index < 0 || !s.on || !s.in_frame)
		return;
	Frame &f = s.frames[s.cur];
	if (m_index >= (int)f.spans.size())
		return;
	f.spans[m_index].q1 = takeQuery();
	// (closed in order; an unclosed span is closed at frameEnd)
	GL.QueryCounter(f.spans[m_index].q1, GL.TIMESTAMP);
	if (!s.stack.empty() && s.stack.back() == m_index)
		s.stack.pop_back();
}

const char *intern(const std::string &name)
{
	static std::set<std::string> names;
	return names.insert(name).first->c_str();
}

std::string report()
{
	auto &s = st();
	std::ostringstream os;
	if (!s.supported) {
		os << "GPU timing is not supported by this driver (needs timer queries)\n";
		return os.str();
	}
	if (s.frames_read == 0) {
		os << "GPU timing: collecting...\n";
		return os.str();
	}
	char buf[160];
	std::snprintf(buf, sizeof(buf), "GPU busy per frame: %.2f ms (%.0f fps if GPU-bound)\n",
			s.total_ms, s.total_ms > 0.0 ? 1000.0 / s.total_ms : 0.0);
	os << buf;
	// tree order: parents before children, siblings by first appearance
	std::vector<const Row *> rows;
	for (auto &r : s.rows)
		rows.push_back(&r.second);
	std::sort(rows.begin(), rows.end(), [] (const Row *a, const Row *b) {
		return a->order < b->order;
	});
	auto line = [&] (int depth, const std::string &name, double ms) {
		const std::string label = std::string(depth * 2, ' ') + name;
		const double pct = s.total_ms > 0.0 ? ms / s.total_ms * 100.0 : 0.0;
		std::snprintf(buf, sizeof(buf), "  %-44s %7.2f ms %5.1f%%\n", label.c_str(), ms, pct);
		os << buf;
	};
	// children in issue order under their parent; a parent with children
	// also shows what its children don't cover (sky, entities, ...)
	std::function<double(const std::string &, int)> emit = [&] (const std::string &parent, int depth) {
		double sum = 0.0;
		for (const Row *r : rows) {
			if (r->depth != depth)
				continue;
			const size_t cut = r->path.rfind('/');
			const std::string p = cut == std::string::npos ? "" : r->path.substr(0, cut);
			if (p != parent)
				continue;
			line(depth, r->name, r->ms);
			const double kids = emit(r->path, depth + 1);
			if (kids > 0.0)
				line(depth + 1, "(rest)", std::max(r->ms - kids, 0.0));
			sum += r->ms;
		}
		return sum;
	};
	emit("", 0);
	return os.str();
}

} // namespace fx::gpu
