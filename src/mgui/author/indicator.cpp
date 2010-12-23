//
// mgui/author/indicator.cpp
// This file is part of Bombono DVD project.
//
// Copyright (c) 2009-2010 Ilya Murav'jov
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
// 

#include <mgui/_pc_.h>

#include "script.h"  // ConsoleMode

#include <mgui/img_utils.h> // Round()
#include <mgui/gettext.h>
#include <mlib/string.h>

static std::string PercentString(double p)
{
    str::stream strm;
    // если хотим PercentString(1) => 01.00%
    //strm.width(5);
    //strm.fill('0');
    //strm << std::fixed;
    //strm.precision(2);

    return (strm << Round(p) << "%").str();
}

void SetPercent(Gtk::ProgressBar& bar, double percent)
{
    if( (0 <= percent) && (percent <= 100.) )
    {
        bar.set_fraction(percent/100.);
        bar.set_text(PercentString(percent));
    }
}

namespace Author
{

struct StageMapT
{
  std::string  stgName;  

         bool  isNeeded; // нужен ли этот этап
         bool  isPassed; // пройден ли

          int  weight;   // веса экспериментально подбирать
       double  dWeight;
} 
StageMap[stLAST] = 
{ 
    { N_("Transcoding videos"),   true, true, 0, 0.0 }, 
    { N_("Rendering menus"),      true, true, 1, 0.0 }, 
    { N_("Generating DVD-Video"), true, true, 3, 0.0 }, 
    { N_("Creating ISO image"),   true, true, 2, 0.0 }, 
    { N_("Burning DVD"),          true, true, 4, 0.0 }, 
};
// текущий этап
Stage CurStage = stNO_STAGE;

void InitStageMap(Mode mod, double trans_ratio)
{
    // * расчет веса для транскодирования
    // ffmpeg приблизительно в 2 раза быстрее реалки
    const double ffmpeg_trans_mult = 0.5;
    // обычный мультипликатор записи dvd;
    // предположительный битрейт DVD-Video (11Mbit/s)
    // равен 1x скорости чтения DVD
    const double dvd_write_mult = 4.0;
    // ~8 для, если транскодировать все
    StageMap[stTRANSCODE].weight = trans_ratio * ffmpeg_trans_mult 
        * dvd_write_mult * StageMap[stBURN].weight;

    StageMap[stDVDAUTHOR].isNeeded = mod != modRENDERING;
    StageMap[stBURN].isNeeded      = mod == modBURN;
    StageMap[stMK_ISO].isNeeded    = mod == modDISK_IMAGE;

    double sum = 0;
    for( int i=0; i<(int)ARR_SIZE(StageMap); i++ )
    {
        StageMapT& s = StageMap[i];
        s.isPassed = false;
        if( s.isNeeded )
            sum += s.weight;
    }
    ASSERT_RTL( sum );

    for( int i=0; i<(int)ARR_SIZE(StageMap); i++ )
        StageMap[i].dWeight = StageMap[i].weight / sum;

    CurStage = stBEG_STAGE;
}

void ExecState::SetIndicator(double percent)
{
    SetPercent(prgBar, percent);
}

void SetStageProgress(double percent, bool is_percent)
{
    if( Execution::ConsoleMode::Flag )
        return;

    double sum = 0;
    for( int i=0; i<(int)ARR_SIZE(StageMap); i++ )
    {
        StageMapT& s = StageMap[i];
        if( s.isNeeded && s.isPassed )
            sum += s.dWeight;
    }
    
    ASSERT( CurStage != stNO_STAGE );
    StageMapT& cur_s = StageMap[CurStage];
    ASSERT( cur_s.isNeeded && !cur_s.isPassed );

    if( !is_percent )
        percent *= 100.;
    sum = sum * 100. + cur_s.dWeight * percent;
    GetES().SetIndicator(sum);
}

std::string StageToStr(Stage st)
{
    StageMapT& s = StageMap[st];
    return gettext(s.stgName.c_str());
}

void SetStage(Stage st)
{
    if( Execution::ConsoleMode::Flag )
        return;

    GetES().SetStatus(StageToStr(st));
    StageMapT& s = StageMap[st];
    ASSERT( s.isNeeded );

    for( int i=stBEG_STAGE; i<st; i++ )
        StageMap[i].isPassed = true;
    s.isPassed = false;

    CurStage = st;
    SetStageProgress(0.);
}

} // namespace Author

