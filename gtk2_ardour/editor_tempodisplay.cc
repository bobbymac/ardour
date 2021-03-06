/*
    Copyright (C) 2002 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <cstdio> // for sprintf, grrr
#include <cstdlib>
#include <cmath>
#include <string>
#include <climits>

#include "pbd/error.h"
#include "pbd/memento_command.h"

#include <gtkmm2ext/utils.h>
#include <gtkmm2ext/gtk_ui.h>

#include "ardour/session.h"
#include "ardour/tempo.h"
#include <gtkmm2ext/doi.h>
#include <gtkmm2ext/utils.h>

#include "canvas/canvas.h"
#include "canvas/item.h"
#include "canvas/line_set.h"

#include "editor.h"
#include "marker.h"
#include "tempo_dialog.h"
#include "rgb_macros.h"
#include "gui_thread.h"
#include "time_axis_view.h"
#include "grid_lines.h"
#include "ui_config.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;
using namespace Gtk;
using namespace Gtkmm2ext;
using namespace Editing;

void
Editor::remove_metric_marks ()
{
	/* don't delete these while handling events, just punt till the GUI is idle */

	for (Marks::iterator x = metric_marks.begin(); x != metric_marks.end(); ++x) {
		delete_when_idle (*x);
	}
	metric_marks.clear ();

	for (Curves::iterator x = tempo_curves.begin(); x != tempo_curves.end(); ++x) {
		delete (*x);
	}
	tempo_curves.clear ();
}
struct CurveComparator {
	bool operator() (TempoCurve const * a, TempoCurve const * b) {
		return a->tempo().sample() < b->tempo().sample();
	}
};
void
Editor::draw_metric_marks (const Metrics& metrics)
{
	char buf[64];
	TempoSection* prev_ts = 0;
	double max_tempo = 0.0;
	double min_tempo = DBL_MAX;

	remove_metric_marks (); // also clears tempo curves

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		const MeterSection *ms;
		TempoSection *ts;

		if ((ms = dynamic_cast<const MeterSection*>(*i)) != 0) {
			snprintf (buf, sizeof(buf), "%g/%g", ms->divisions_per_bar(), ms->note_divisor ());
			if (ms->position_lock_style() == MusicTime) {
				metric_marks.push_back (new MeterMarker (*this, *meter_group, UIConfiguration::instance().color ("meter marker music"), buf,
									 *(const_cast<MeterSection*>(ms))));
			} else {
				metric_marks.push_back (new MeterMarker (*this, *meter_group, UIConfiguration::instance().color ("meter marker"), buf,
									 *(const_cast<MeterSection*>(ms))));
			}
		} else if ((ts = dynamic_cast<TempoSection*>(*i)) != 0) {

			max_tempo = max (max_tempo, ts->note_types_per_minute());
			max_tempo = max (max_tempo, ts->end_note_types_per_minute());
			min_tempo = min (min_tempo, ts->note_types_per_minute());
			min_tempo = min (min_tempo, ts->end_note_types_per_minute());
			uint32_t const tc_color = UIConfiguration::instance().color ("tempo curve");

			tempo_curves.push_back (new TempoCurve (*this, *tempo_group, tc_color,
								*(const_cast<TempoSection*>(ts)), ts->sample(), false));

			const std::string tname (X_(""));
			if (ts->position_lock_style() == MusicTime) {
				metric_marks.push_back (new TempoMarker (*this, *tempo_group, UIConfiguration::instance().color ("tempo marker music"), tname,
								 *(const_cast<TempoSection*>(ts))));
			} else {
				metric_marks.push_back (new TempoMarker (*this, *tempo_group, UIConfiguration::instance().color ("tempo marker"), tname,
								 *(const_cast<TempoSection*>(ts))));
			}
			if (prev_ts && abs (prev_ts->end_note_types_per_minute() - ts->note_types_per_minute()) < 1.0) {
				metric_marks.back()->set_points_color (UIConfiguration::instance().color ("tempo marker music"));
			} else {
				metric_marks.back()->set_points_color (UIConfiguration::instance().color ("tempo marker"));
			}
			prev_ts = ts;
		}

	}
	tempo_curves.sort (CurveComparator());

	const double min_tempo_range = 5.0;
	const double tempo_delta = fabs (max_tempo - min_tempo);

	if (tempo_delta < min_tempo_range) {
		max_tempo += min_tempo_range - tempo_delta;
		min_tempo += tempo_delta - min_tempo_range;
	}

	for (Curves::iterator x = tempo_curves.begin(); x != tempo_curves.end(); ) {
		Curves::iterator tmp = x;
		(*x)->set_max_tempo (max_tempo);
		(*x)->set_min_tempo (min_tempo);
		++tmp;
		if (tmp != tempo_curves.end()) {
			(*x)->set_position ((*x)->tempo().sample(), (*tmp)->tempo().sample());
		} else {
			(*x)->set_position ((*x)->tempo().sample(), UINT32_MAX);
		}

		if (!(*x)->tempo().active()) {
			(*x)->hide();
		} else {
			(*x)->show();
		}

		++x;
	}

	for (Marks::iterator x = metric_marks.begin(); x != metric_marks.end(); ++x) {
		TempoMarker* tempo_marker;

		if ((tempo_marker = dynamic_cast<TempoMarker*> (*x)) != 0) {
			tempo_marker->update_height_mark ((tempo_marker->tempo().note_types_per_minute() - min_tempo) / max (10.0, max_tempo - min_tempo));
		}
	}
}


void
Editor::tempo_map_changed (const PropertyChange& /*ignored*/)
{
	if (!_session) {
		return;
	}

	ENSURE_GUI_THREAD (*this, &Editor::tempo_map_changed, ignored);

	compute_bbt_ruler_scale (_leftmost_sample, _leftmost_sample + current_page_samples());

	_session->tempo_map().apply_with_metrics (*this, &Editor::draw_metric_marks); // redraw metric markers
	update_tempo_based_rulers ();

	maybe_draw_grid_lines ();
}

void
Editor::tempometric_position_changed (const PropertyChange& /*ignored*/)
{
	if (!_session) {
		return;
	}

	ENSURE_GUI_THREAD (*this, &Editor::tempo_map_changed);

	TempoSection* prev_ts = 0;
	double max_tempo = 0.0;
	double min_tempo = DBL_MAX;

	for (Marks::iterator x = metric_marks.begin(); x != metric_marks.end(); ++x) {
		TempoMarker* tempo_marker;
		MeterMarker* meter_marker;
		TempoSection *ts;
		const MeterSection *ms;

		if ((tempo_marker = dynamic_cast<TempoMarker*> (*x)) != 0) {
			if ((ts = &tempo_marker->tempo()) != 0) {

				tempo_marker->set_position (ts->sample ());

				if (prev_ts && abs (prev_ts->end_note_types_per_minute() - ts->note_types_per_minute()) < 1.0) {
					tempo_marker->set_points_color (UIConfiguration::instance().color ("tempo marker music"));
				} else {
					tempo_marker->set_points_color (UIConfiguration::instance().color ("tempo marker"));
				}

				max_tempo = max (max_tempo, ts->note_types_per_minute());
				max_tempo = max (max_tempo, ts->end_note_types_per_minute());
				min_tempo = min (min_tempo, ts->note_types_per_minute());
				min_tempo = min (min_tempo, ts->end_note_types_per_minute());

				prev_ts = ts;
			}
		}
		if ((meter_marker = dynamic_cast<MeterMarker*> (*x)) != 0) {
			if ((ms = &meter_marker->meter()) != 0) {
				meter_marker->set_position (ms->sample ());
			}
		}
	}

	tempo_curves.sort (CurveComparator());

	const double min_tempo_range = 5.0;
	const double tempo_delta = fabs (max_tempo - min_tempo);

	if (tempo_delta < min_tempo_range) {
		max_tempo += min_tempo_range - tempo_delta;
		min_tempo += tempo_delta - min_tempo_range;
	}

	for (Curves::iterator x = tempo_curves.begin(); x != tempo_curves.end(); ) {
		Curves::iterator tmp = x;
		(*x)->set_max_tempo (max_tempo);
		(*x)->set_min_tempo (min_tempo);
		++tmp;
		if (tmp != tempo_curves.end()) {
			(*x)->set_position ((*x)->tempo().sample(), (*tmp)->tempo().sample());
		} else {
			(*x)->set_position ((*x)->tempo().sample(), UINT32_MAX);
		}

		if (!(*x)->tempo().active()) {
			(*x)->hide();
		} else {
			(*x)->show();
		}

		++x;
	}

	for (Marks::iterator x = metric_marks.begin(); x != metric_marks.end(); ++x) {
		TempoMarker* tempo_marker;
		if ((tempo_marker = dynamic_cast<TempoMarker*> (*x)) != 0) {
			tempo_marker->update_height_mark ((tempo_marker->tempo().note_types_per_minute() - min_tempo) / max (max_tempo - min_tempo, 10.0));
		}
	}

	compute_bbt_ruler_scale (_leftmost_sample, _leftmost_sample + current_page_samples());

	update_tempo_based_rulers ();

	maybe_draw_grid_lines ();
}

void
Editor::redisplay_grid (bool immediate_redraw)
{
	if (!_session) {
		return;
	}

	if (immediate_redraw) {

		update_tempo_based_rulers ();

		update_grid();
		
	} else {
		Glib::signal_idle().connect (sigc::bind_return (sigc::bind (sigc::mem_fun (*this, &Editor::redisplay_grid), true), false));
	}
}
void
Editor::tempo_curve_selected (TempoSection* ts, bool yn)
{
	if (ts == 0) {
		return;
	}

	for (Curves::iterator x = tempo_curves.begin(); x != tempo_curves.end(); ++x) {
		if (&(*x)->tempo() == ts) {
			if (yn) {
				(*x)->set_color_rgba (UIConfiguration::instance().color ("location marker"));
			} else {
				(*x)->set_color_rgba (UIConfiguration::instance().color ("tempo curve"));
			}
			break;
		}
	}
}

/* computes a grid starting a beat before and ending a beat after leftmost and rightmost respectively */
void
Editor::compute_current_bbt_points (std::vector<TempoMap::BBTPoint>& grid, samplepos_t leftmost, samplepos_t rightmost)
{
	if (!_session) {
		return;
	}

	/* prevent negative values of leftmost from creeping into tempomap
	 */
	const double lower_beat = floor (max (0.0, _session->tempo_map().beat_at_sample (leftmost))) - 1.0;
	switch (bbt_ruler_scale) {

	case bbt_show_beats:
	case bbt_show_ticks:
	case bbt_show_ticks_detail:
	case bbt_show_ticks_super_detail:
		_session->tempo_map().get_grid (grid, max (_session->tempo_map().sample_at_beat (lower_beat), (samplepos_t) 0), rightmost);
		break;

	case bbt_show_1:
		_session->tempo_map().get_grid (grid, max (_session->tempo_map().sample_at_beat (lower_beat), (samplepos_t) 0), rightmost, 1);
		break;

	case bbt_show_4:
		_session->tempo_map().get_grid (grid, max (_session->tempo_map().sample_at_beat (lower_beat), (samplepos_t) 0), rightmost, 4);
		break;

	case bbt_show_16:
		_session->tempo_map().get_grid (grid, max (_session->tempo_map().sample_at_beat (lower_beat), (samplepos_t) 0), rightmost, 16);
		break;

	case bbt_show_64:
		_session->tempo_map().get_grid (grid, max (_session->tempo_map().sample_at_beat (lower_beat), (samplepos_t) 0), rightmost, 64);
		break;

	default:
		/* bbt_show_many */
		_session->tempo_map().get_grid (grid, max (_session->tempo_map().sample_at_beat (lower_beat), (samplepos_t) 0), rightmost, 128);
		break;
	}
}

void
Editor::hide_grid_lines ()
{
	if (grid_lines) {
		grid_lines->hide();
	}
}

void
Editor::maybe_draw_grid_lines ()
{
	if ( _session == 0 ) {
		return;
	}

	if (grid_lines == 0) {
		grid_lines = new GridLines (time_line_group, ArdourCanvas::LineSet::Vertical);
	}

	grid_marks.clear();
	samplepos_t rightmost_sample = _leftmost_sample + current_page_samples();

	if ( grid_musical() ) {
		 metric_get_bbt (grid_marks, _leftmost_sample, rightmost_sample, 12);
	} else if (_grid_type== GridTypeSmpte) {
		 metric_get_timecode (grid_marks, _leftmost_sample, rightmost_sample, 12);
	} else if (_grid_type == GridTypeSamples) {
		metric_get_samples (grid_marks, _leftmost_sample, rightmost_sample, 12);
	} else if (_grid_type == GridTypeMinSec) {
		metric_get_minsec (grid_marks, _leftmost_sample, rightmost_sample, 12);
	}

	grid_lines->draw ( grid_marks );
	grid_lines->show();
}

void
Editor::mouse_add_new_tempo_event (samplepos_t sample)
{
	if (_session == 0) {
		return;
	}

	TempoMap& map(_session->tempo_map());

	begin_reversible_command (_("add tempo mark"));
	const double pulse = map.exact_qn_at_sample (sample, get_grid_music_divisions (0)) / 4.0;

	if (pulse > 0.0) {
		XMLNode &before = map.get_state();
		/* add music-locked ramped (?) tempo using the bpm/note type at sample*/
		map.add_tempo (map.tempo_at_sample (sample), pulse, 0, MusicTime);

		XMLNode &after = map.get_state();
		_session->add_command(new MementoCommand<TempoMap>(map, &before, &after));
		commit_reversible_command ();
	}

	//map.dump (cerr);
}

void
Editor::mouse_add_new_meter_event (samplepos_t sample)
{
	if (_session == 0) {
		return;
	}


	TempoMap& map(_session->tempo_map());
	MeterDialog meter_dialog (map, sample, _("add"));

	switch (meter_dialog.run ()) {
	case RESPONSE_ACCEPT:
		break;
	default:
		return;
	}

	double bpb = meter_dialog.get_bpb ();
	bpb = max (1.0, bpb); // XXX is this a reasonable limit?

	double note_type = meter_dialog.get_note_type ();

	Timecode::BBT_Time requested;
	meter_dialog.get_bbt_time (requested);

	const double al_sample = map.sample_at_bbt (requested);
	begin_reversible_command (_("add meter mark"));
	XMLNode &before = map.get_state();

	if (meter_dialog.get_lock_style() == MusicTime) {
		map.add_meter (Meter (bpb, note_type), requested, 0, MusicTime);
	} else {
		map.add_meter (Meter (bpb, note_type), requested, al_sample, AudioTime);
	}

	_session->add_command(new MementoCommand<TempoMap>(map, &before, &map.get_state()));
	commit_reversible_command ();

	//map.dump (cerr);
}

void
Editor::remove_tempo_marker (ArdourCanvas::Item* item)
{
	ArdourMarker* marker;
	TempoMarker* tempo_marker;

	if ((marker = reinterpret_cast<ArdourMarker *> (item->get_data ("marker"))) == 0) {
		fatal << _("programming error: tempo marker canvas item has no marker object pointer!") << endmsg;
		abort(); /*NOTREACHED*/
	}

	if ((tempo_marker = dynamic_cast<TempoMarker*> (marker)) == 0) {
		fatal << _("programming error: marker for tempo is not a tempo marker!") << endmsg;
		abort(); /*NOTREACHED*/
	}

	if (!tempo_marker->tempo().locked_to_meter() && tempo_marker->tempo().active()) {
		Glib::signal_idle().connect (sigc::bind (sigc::mem_fun(*this, &Editor::real_remove_tempo_marker), &tempo_marker->tempo()));
	}
}

void
Editor::edit_meter_section (MeterSection* section)
{
	MeterDialog meter_dialog (_session->tempo_map(), *section, _("done"));

	switch (meter_dialog.run()) {
	case RESPONSE_ACCEPT:
		break;
	default:
		return;
	}

	double bpb = meter_dialog.get_bpb ();
	bpb = max (1.0, bpb); // XXX is this a reasonable limit?

	double const note_type = meter_dialog.get_note_type ();
	const Meter meter (bpb, note_type);

	Timecode::BBT_Time when;
	meter_dialog.get_bbt_time (when);
	const samplepos_t sample = _session->tempo_map().sample_at_bbt (when);
	const PositionLockStyle pls = (meter_dialog.get_lock_style() == AudioTime) ? AudioTime : MusicTime;

	begin_reversible_command (_("replace meter mark"));
	XMLNode &before = _session->tempo_map().get_state();

	_session->tempo_map().replace_meter (*section, meter, when, sample, pls);

	XMLNode &after = _session->tempo_map().get_state();
	_session->add_command(new MementoCommand<TempoMap>(_session->tempo_map(), &before, &after));
	commit_reversible_command ();
}

void
Editor::edit_tempo_section (TempoSection* section)
{
	TempoDialog tempo_dialog (_session->tempo_map(), *section, _("done"));

	switch (tempo_dialog.run ()) {
	case RESPONSE_ACCEPT:
		break;
	default:
		return;
	}

	double bpm = tempo_dialog.get_bpm ();
	double end_bpm = tempo_dialog.get_end_bpm ();
	double nt = tempo_dialog.get_note_type ();
	bpm = max (0.01, bpm);
	const Tempo tempo (bpm, nt, end_bpm);

	Timecode::BBT_Time when;
	tempo_dialog.get_bbt_time (when);

	begin_reversible_command (_("replace tempo mark"));
	XMLNode &before = _session->tempo_map().get_state();

	if (tempo_dialog.get_lock_style() == AudioTime) {
		samplepos_t const f = _session->tempo_map().predict_tempo_position (section, when).second;
		_session->tempo_map().replace_tempo (*section, tempo, 0.0, f, AudioTime);
	} else {
		double const p = _session->tempo_map().predict_tempo_position (section, when).first;
		_session->tempo_map().replace_tempo (*section, tempo, p, 0, MusicTime);
	}

	XMLNode &after = _session->tempo_map().get_state();
	_session->add_command (new MementoCommand<TempoMap>(_session->tempo_map(), &before, &after));
	commit_reversible_command ();
}

void
Editor::edit_tempo_marker (TempoMarker& tm)
{
	edit_tempo_section (&tm.tempo());
}

void
Editor::edit_meter_marker (MeterMarker& mm)
{
	edit_meter_section (&mm.meter());
}

gint
Editor::real_remove_tempo_marker (TempoSection *section)
{
	begin_reversible_command (_("remove tempo mark"));
	XMLNode &before = _session->tempo_map().get_state();
	_session->tempo_map().remove_tempo (*section, true);
	XMLNode &after = _session->tempo_map().get_state();
	_session->add_command(new MementoCommand<TempoMap>(_session->tempo_map(), &before, &after));
	commit_reversible_command ();

	return FALSE;
}

void
Editor::remove_meter_marker (ArdourCanvas::Item* item)
{
	ArdourMarker* marker;
	MeterMarker* meter_marker;

	if ((marker = reinterpret_cast<ArdourMarker *> (item->get_data ("marker"))) == 0) {
		fatal << _("programming error: meter marker canvas item has no marker object pointer!") << endmsg;
		abort(); /*NOTREACHED*/
	}

	if ((meter_marker = dynamic_cast<MeterMarker*> (marker)) == 0) {
		fatal << _("programming error: marker for meter is not a meter marker!") << endmsg;
		abort(); /*NOTREACHED*/
	}

	if (!meter_marker->meter().initial()) {
	  Glib::signal_idle().connect (sigc::bind (sigc::mem_fun(*this, &Editor::real_remove_meter_marker), &meter_marker->meter()));
	}
}

gint
Editor::real_remove_meter_marker (MeterSection *section)
{
	begin_reversible_command (_("remove tempo mark"));
	XMLNode &before = _session->tempo_map().get_state();
	_session->tempo_map().remove_meter (*section, true);
	XMLNode &after = _session->tempo_map().get_state();
	_session->add_command(new MementoCommand<TempoMap>(_session->tempo_map(), &before, &after));
	commit_reversible_command ();

	return FALSE;
}
