// Luanti "fx" fork: the in-game mood tuner (pause menu → Mood)
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A formspec of sliders over the TUNE entries of params.def. Sliders write
// the fx_* settings directly, so the tuner is just another view of the
// regular settings (Settings → Mood Renderer shows the same values).

#include "renderer.h"

#include "gettext.h"
#include "settings.h"
#include "util/string.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace fx
{

static std::vector<const ParamInfo *> sliders()
{
	std::vector<const ParamInfo *> out;
	for (const auto &i : paramInfo())
		if (i.tune && i.type == ParamType::Float)
			out.push_back(&i);
	return out;
}

static std::vector<const ParamInfo *> toggles()
{
	std::vector<const ParamInfo *> out;
	for (const auto &i : paramInfo())
		if (i.tune && i.type == ParamType::Bool)
			out.push_back(&i);
	return out;
}

// scroll positions of the two lists, kept across rebuilds of the form
static int s_scroll_sliders = 0, s_scroll_checks = 0;

static int sliderPos(const ParamInfo &p, float v)
{
	return core::round32(std::clamp((v - p.min) / (p.max - p.min), 0.f, 1.f) * 1000.f);
}

std::string tunerFormspec(const std::string &status)
{
	const auto params = sliders();
	const auto moods = listMoods();
	const std::string current = g_settings->get("fx_mood");
	const auto checks = toggles();
	const int per_col = (int)(params.size() + 1) / 2;
	const float row = 0.72f;
	const int check_cols = 4, check_rows = (int)(checks.size() + check_cols - 1) / check_cols;
	const float check_row = 0.5f;
	// both lists scroll: the form stays the same size however many there are
	const float slider_view = 8.6f, check_view = 2.6f;
	const float slider_h = per_col * row + 0.2f, check_h = check_rows * check_row + 0.2f;
	const float y_checks = 1.7f + slider_view + 0.35f;
	const float h = y_checks + 0.45f + check_view + 1.3f;

	std::ostringstream os;
	os << std::fixed;
	os.precision(2);
	os << "formspec_version[6]size[17," << h << "]"
		<< "bgcolor[#00000080;neither]"
		<< "label[0.4,0.5;" << strgettext("Mood tuner") << "]"
		<< "label[0.4,1.15;" << strgettext("Mood:") << "]";
	std::string list;
	int index = 1;
	for (size_t i = 0; i < moods.size(); i++) {
		std::string esc = moods[i];
		str_formspec_escape(esc);
		list += (i ? "," : "") + esc;
		if (moods[i] == current)
			index = (int)i + 1;
	}
	std::string note = tunerNote();
	if (note.empty())
		note = strgettext("Cave, night and underwater targets blend in automatically.");
	str_formspec_escape(note);
	os << "dropdown[1.6,0.85;4,0.6;fx_mood_select;" << list << ";" << index << ";false]"
		<< "label[6,1.15;" << note << "]";

	const int slider_max = std::max(0, (int)std::ceil((slider_h - slider_view) * 10.f));
	os << "scrollbaroptions[min=0;max=" << slider_max << ";smallstep=5;largestep=40;arrows=hide]"
		<< "scrollbar[16.45,1.7;0.3," << slider_view << ";vertical;fx_scroll_sliders;"
		<< std::min(s_scroll_sliders, slider_max) << "]"
		<< "scroll_container[0,1.7;16.4," << slider_view << ";fx_scroll_sliders;vertical;0.1]";
	os << "scrollbaroptions[min=0;max=1000;smallstep=10;largestep=100;arrows=hide]";
	for (size_t i = 0; i < params.size(); i++) {
		const ParamInfo &p = *params[i];
		const int col = (int)i / per_col, r = (int)i % per_col;
		const float x = 0.4f + col * 8.3f, y = 0.2f + r * row;
		const float v = g_settings->getFloat(p.setting);
		// no value in the label: the form is not rebuilt while dragging (that
		// would drop the drag), so a number here would go stale
		os << "label[" << x << "," << (y + 0.25f) << ";" << p.label << "]"
			<< "scrollbar[" << (x + 4.1f) << "," << y << ";3.8,0.45;horizontal;"
			<< p.setting << ";" << sliderPos(p, v) << "]";
	}
	os << "scroll_container_end[]";
	{
		// escaped: a ';' in the text would end the element early
		std::string head = strgettext("Effects (off removes the shader work, * applies on rejoin):");
		str_formspec_escape(head);
		os << "label[0.4," << (y_checks) << ";" << head << "]";
	}
	const int check_max = std::max(0, (int)std::ceil((check_h - check_view) * 10.f));
	os << "scrollbaroptions[min=0;max=" << check_max << ";smallstep=5;largestep=20;arrows=hide]"
		<< "scrollbar[16.45," << (y_checks + 0.3f) << ";0.3," << check_view << ";vertical;fx_scroll_checks;"
		<< std::min(s_scroll_checks, check_max) << "]"
		<< "scroll_container[0," << (y_checks + 0.3f) << ";16.4," << check_view
		<< ";fx_scroll_checks;vertical;0.1]";
	for (size_t i = 0; i < checks.size(); i++) {
		const ParamInfo &p = *checks[i];
		const int col = (int)i % check_cols, r = (int)i / check_cols;
		const float x = 0.4f + col * 4.15f, y = 0.25f + r * check_row;
		std::string label = p.label;
		if (p.apply == Apply::RESTART)
			label += " *";
		str_formspec_escape(label);
		os << "checkbox[" << x << "," << y << ";" << p.setting << ";" << label << ";"
			<< (g_settings->getBool(p.setting) ? "true" : "false") << "]";
	}
	os << "scroll_container_end[]";
	const float yb = h - 0.95f;
	std::string esc_status = status;
	str_formspec_escape(esc_status);
	os << "field[0.4," << yb << ";4,0.65;fx_save_name;;" << current << "]"
		<< "field_close_on_enter[fx_save_name;false]"
		<< "button[4.5," << yb << ";3,0.65;fx_save;" << strgettext("Save as mood") << "]"
		<< "label[7.8," << (yb + 0.33f) << ";" << esc_status << "]"
		<< "button_exit[13.6," << yb << ";3,0.65;fx_done;" << strgettext("Done") << "]";
	return os.str();
}

bool tunerFields(const std::unordered_map<std::string, std::string> &fields, std::string &status)
{
	bool refresh = false;
	// remember where the lists were scrolled to
	for (auto [name, pos] : {std::make_pair("fx_scroll_sliders", &s_scroll_sliders),
			std::make_pair("fx_scroll_checks", &s_scroll_checks)}) {
		auto it = fields.find(name);
		if (it != fields.end() && it->second.size() > 4)
			*pos = mystoi(it->second.substr(4));
	}
	// A mood switch first, applied now, so the rebuilt form shows its values
	// and no slider below writes the previous mood's value back.
	if (auto it = fields.find("fx_mood_select"); it != fields.end() &&
			it->second != g_settings->get("fx_mood")) {
		g_settings->set("fx_mood", it->second);
		if (Renderer *r = renderer())
			r->applyMood(it->second);
		status = strgettext("Mood applied");
		return true;
	}
	// checkboxes: a toggle reports only itself
	for (const ParamInfo *p : toggles()) {
		auto it = fields.find(p->setting);
		if (it == fields.end())
			continue;
		g_settings->setBool(p->setting, it->second == "true");
		status = p->apply == Apply::RESTART ? strgettext("Applies when you next join a world")
			: strgettext("Updated");
		return true;
	}
	// Every event reports every scrollbar; the one being moved says "CHG:".
	// The others ("VAL:") are left alone, so values set elsewhere are not
	// snapped to the slider grid or reverted to a stale position.
	for (const ParamInfo *p : sliders()) {
		auto it = fields.find(p->setting);
		if (it == fields.end() || !str_starts_with(it->second, "CHG:"))
			continue;
		const int pos = mystoi(it->second.substr(4));
		if (pos != sliderPos(*p, g_settings->getFloat(p->setting)))
			g_settings->setFloat(p->setting, p->min + pos / 1000.f * (p->max - p->min));
	}
	if (fields.count("fx_save")) {
		auto it = fields.find("fx_save_name");
		const std::string path = it == fields.end() ? "" : saveMood(it->second);
		status = path.empty() ? strgettext("Could not save (name must be letters, digits, _ or -)")
			: strgettext("Saved to ") + path;
		refresh = true;
	}
	return refresh;
}

} // namespace fx
