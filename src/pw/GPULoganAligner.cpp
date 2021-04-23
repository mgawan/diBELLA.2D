/* Created by Saliya Ekanayake on 2019-07-05 and modified by Giulia Guidi on 4/14/2021. */

#include "../../include/pw/GPULoganAligner.hpp"
#include "../../LoganGPU/RunLoganAligner.hpp" 	// Call to aligner
//#include "../../LoganGPU/interface.hpp"		// LoganResult struct and SeedInterface struct

#define MIN_OV_LEN 10000

char 
complementbase(char n)
{   
    switch(n)
    {   
    case 'A':
        return 'T';
    case 'T':
        return 'A';
    case 'G':
        return 'C';
    case 'C':
        return 'G';
    }   
    assert(false);
    return ' ';
} 

std::string
reversecomplement(const std::string& seq) {

	std::string cpyseq = seq;
	std::reverse(cpyseq.begin(), cpyseq.end());

	std::transform(
		std::begin(cpyseq),
		std::end  (cpyseq),
		std::begin(cpyseq),
	complementbase);

	return cpyseq;
}

void PostAlignDecision(const LoganAlignmentInfo& ai, bool& passed, float& ratioScoreOverlap, 
	uint32_t& overhang, uint32_t& overhangT, uint32_t& overlap, const bool noAlign)
{
	// {begin/end}Position{V/H}: Returns the begin/end position of the seed in the seqVs (vertical/horizonral direction)
	// these four return seqan:Tposition objects
	int begpV = ai.begSeedV;
	int endpV = ai.endSeedV;
	int begpH = ai.begSeedH;
	int endpH = ai.endSeedH;

	unsigned short int overlapLenH = ai.seq_h_seed_length;
	unsigned short int overlapLenV = ai.seq_v_seed_length;

	unsigned short int rlenH = ai.seq_h_length;
	unsigned short int rlenV = ai.seq_v_length;

	unsigned short int minLeft  = min(begpV, begpH);
	unsigned short int minRight = min(rlenV - endpV, rlenH - endpH);

	overlap = minLeft + minRight + (overlapLenV + overlapLenH) / 2;

#ifndef FIXEDTHR
	float myThr = (1 - DELTACHERNOFF) * (ratioScoreOverlap * (float)overlap);

	// Contained overlaps removed for now, reintroduce them later
	bool contained = false;
	bool chimeric  = false; 

	// seqH is column entry and seqV is row entry, for each column, we iterate over seqHs
	int seqH = ai.seq_v_g_idx, seqV = ai.seq_h_g_idx;
	
	// Reserve length/position if rc [x]
	if(ai.rc)
	{
		uint tmp = begpV;
		begpV = rlenV - endpV;
		endpV = rlenV - tmp;
	}

	if((begpH == 0 & rlenH-endpH == 0) || (begpV == 0 & rlenV-endpV == 0))
		contained = true;
	
	if(!contained)
	{
		// If noAlign is false, set passed to false if the score isn't good enough
		if(!noAlign)
		{
			if((float)ai.xscore < myThr || overlap < MIN_OV_LEN) passed = false;
			else passed = true;
		}

		if(passed)
		{
			uint32_t direction, directionT;
			uint32_t suffix, suffixT;

			// !reverse complement
			if(!ai.rc)
			{
				if(begpH > begpV)
				{
					direction  = 1;
					directionT = 2;

					suffix  = rlenV - endpV;
					suffixT = begpH;
				}	
				else
				{
					direction  = 2;
					directionT = 1;

					suffix  = rlenH - endpH;
					suffixT = begpV;
				} 
			}
			else
			{
				if((begpV > 0) & (begpH > 0) & (rlenV-endpV == 0) & (rlenV-endpV == 0))
				{
					direction  = 0;
					directionT = 0;

					suffix  = begpV; // seqV == 2
					suffixT = begpH; // seqV == 2			
				}
				else
				{
					direction  = 3;
					directionT = 3;

					suffix  = rlenV - endpV; // seqV == 1, seqH == 2	
					suffixT = rlenH - endpH; // seqV == 1, seqH == 2		
				}
			}
			overhang  = suffix  << 2 | direction;
			overhangT = suffixT << 2 | directionT;
		} // if(passed)
	} // if(!contained)
		
#else
	if(ai.xscore >= FIXEDTHR)
		passed = true;
#endif
}

GPULoganAligner::GPULoganAligner(
    ScoringScheme scoring_scheme,
    ushort seed_length, int xdrop, int seed_count):
    PairwiseFunction(),
    scoring_scheme(scoring_scheme),
    seed_length(seed_length), xdrop(xdrop), seed_count(seed_count){
}

void GPULoganAligner::apply(
    uint64_t l_col_idx, uint64_t g_col_idx,
    uint64_t l_row_idx, uint64_t g_row_idx,
    seqan::Dna5String *seqH, seqan::Dna5String *seqV, ushort k,
    dibella::CommonKmers &cks, std::stringstream& ss)
{
    // ...
}

// @NOTE This is hard-coded to the number of seeds being <= 2
void
GPULoganAligner::apply_batch
(
    seqan::StringSet<seqan::Dna5String> &seqsh,
	seqan::StringSet<seqan::Dna5String> &seqsv,
	uint64_t *lids,
	uint64_t col_offset,
	uint64_t row_offset,
    PSpMat<dibella::CommonKmers>::ref_tuples *mattuples,
    std::ofstream &lfs,
	const bool noAlign,
	ushort k,
	uint64_t nreads,
    float ratioScoreOverlap, // GGGG: this is my ratioScoreOverlap variable change name later
    int debugThr
)
{
	// seqan::ExecutionPolicy<seqan::Parallel, seqan::Vectorial> exec_policy;

	int numThreads = 1;
	#ifdef THREADED
	#pragma omp parallel
    {
      	numThreads = omp_get_num_threads();
    }
	#endif

	uint64_t npairs = seqan::length(seqsh);
	// setNumThreads(exec_policy, numThreads);
	
	lfs << "processing batch of size " << npairs << " with " << numThreads << " threads " << std::endl;

	// for multiple seeds we store the seed with the highest identity
	LoganAlignmentInfo *ai = new LoganAlignmentInfo[npairs];
	std::pair<ushort, ushort> *seedlens = new std::pair<ushort, ushort>[npairs];

	std::vector<string> seqHs;
	std::vector<string> seqVs;

	std::vector<SeedInterface> seeds;
	std::vector<LoganResult> xscores;

	/* GGGG: seed_count is hardcoded here (2) */
	for(int count = 0; count < seed_count; ++count)
	{
		auto start_time = std::chrono::system_clock::now();
	
		// @GGGG: keep the order for the post alignment evaluation (measure slowdown)
		// #pragma omp parallel for 
		for (uint64_t i = 0; i < npairs; ++i) // I acculate sequences for GPU batch alignment
		{
			// Init result
			LoganResult localRes; 

			// Get seed location
			dibella::CommonKmers *cks = std::get<2>(mattuples[lids[i]]);

		#ifdef TWOSEED
			ushort LocaSeedInterfaceVOffset =
				(count == 0) ? cks->first.first : cks->second.first;
			ushort LocaSeedInterfaceHOffset =
				(count == 0) ? cks->first.second : cks->second.second;
		#else
			// GGGG: TODO check reverse complement
			ushort LocaSeedInterfaceVOffset = cks.pos[0].first;
			ushort LocaSeedInterfaceHOffset = cks.pos[0].second;
		#endif

			// Get sequences
			std::string seqH;
			std::string seqV;

			seqan::assign(seqH, seqsh[i]);
			seqan::assign(seqV, seqsv[i]);

			uint lenH = seqH.length();
			uint lenV = seqV.length();

			// Get seed string
			std::string seedH = seqH.substr(LocaSeedInterfaceHOffset, seed_length);
			std::string seedV = seqV.substr(LocaSeedInterfaceVOffset, seed_length);

			std::string twinseedH = reversecomplement(seedH);

			if(twinseedH == seedV)
			{
				std::string twinseqH(seqH);

				std::reverse(std::begin(twinseqH), std::end(twinseqH));
				std::transform(std::begin(twinseqH), std::end(twinseqH), std::begin(twinseqH), complementbase);

				LocaSeedInterfaceHOffset = twinseqH.length(); - LocaSeedInterfaceHOffset - seed_length;

				SeedInterface seed(LocaSeedInterfaceHOffset, LocaSeedInterfaceVOffset, LocaSeedInterfaceHOffset + seed_length, LocaSeedInterfaceVOffset + seed_length);

				// GGGG: here only accumulate stuff for the GPUs, don't perform alignment
				seeds.push_back(seed);
				seqVs.push_back(seqV);
				seqHs.push_back(twinseqH);

				localRes.rc = true;
				xscores.push_back(localRes);
			}
			else
			{
				SeedInterface seed(LocaSeedInterfaceHOffset, LocaSeedInterfaceVOffset, LocaSeedInterfaceHOffset + seed_length, LocaSeedInterfaceVOffset + seed_length);

				// GGGG: here only accumulate stuff for the GPUs, don't perform alignment
				seeds.push_back(seed);
				seqVs.push_back(seqV);
				seqHs.push_back(seqH);

				localRes.rc = false;
				xscores.push_back(localRes);
			}
		}

		auto end_time = std::chrono::system_clock::now();
    	add_time("XA:LoganPreprocess", (ms_t(end_time - start_time)).count());

		start_time = std::chrono::system_clock::now();

		// Call LOGAN only if noAlign is false
		if(!noAlign) 
		{ 
			// @GGGG-TODO: Check the parameter
			RunLoganAlign(seqHs, seqVs, seeds, xscores, xdrop, seed_length);
		}

		end_time = std::chrono::system_clock::now();
    	add_time("XA:LoganAlign", (ms_t(end_time - start_time)).count());

		start_time = std::chrono::system_clock::now();
		
		// @GGGG-TODO: this runs everything twice right now; pretty sure I can avoid this using CCS, I can add an option also run some benchmark
		// Compute stats
		if (count == 0)	// overwrite in the first seed
		{
			// @GGGG: keep the order for the post alignment evaluation (measure slowdown)
			// #pragma omp parallel for 
			for (uint64_t i = 0; i < npairs; ++i)
			{
				ai[i].xscore = xscores[i].score; 
				ai[i].rc     = xscores[i].rc;

                ai[i].begSeedH = xscores[i].begSeedH; 
                ai[i].endSeedH = xscores[i].endSeedH; 
                ai[i].begSeedV = xscores[i].begSeedV; 
                ai[i].endSeedV = xscores[i].endSeedV; 

				ai[i].seq_h_length = seqan::length(seqsh[i]);
				ai[i].seq_v_length = seqan::length(seqsv[i]);

				// this is a bit redundant since we can extract it from seed
				ai[i].seq_h_seed_length = ai[i].endSeedH - ai[i].begSeedH;
				ai[i].seq_v_seed_length = ai[i].endSeedV - ai[i].begSeedV;

				ai[i].seq_h_g_idx = col_offset + std::get<1>(mattuples[lids[i]]);
    			ai[i].seq_v_g_idx = row_offset + std::get<0>(mattuples[lids[i]]);
			}
		}
		else
		{
			// @GGGG: keep the order for the post alignment evaluation (measure slowdown)
			// #pragma omp parallel for 
			for (uint64_t i = 0; i < npairs; ++i)
			{
				if (xscores[i].score > ai[i].xscore)
				{
					ai[i].xscore = xscores[i].score;
					ai[i].rc     = xscores[i].rc;

                    ai[i].begSeedH = xscores[i].begSeedH; 
                    ai[i].endSeedH = xscores[i].endSeedH; 
                    ai[i].begSeedV = xscores[i].begSeedV; 
                    ai[i].endSeedV = xscores[i].endSeedV; 

					// @GGGG: this is a bit redundant since we can extract it from seed
					ai[i].seq_h_seed_length = ai[i].endSeedH - ai[i].begSeedH; //ai[i].SeedInterface.endPositionH - ai[i].SeedInterface.beginPositionH;
					ai[i].seq_v_seed_length = ai[i].endSeedV - ai[i].begSeedV; //ai[i].SeedInterface.endPositionV - ai[i].SeedInterface.beginPositionV;
				}
			}
		}

		end_time = std::chrono::system_clock::now();
    	add_time("XA:ComputeStats", (ms_t(end_time - start_time)).count());
	}

	auto start_time = std::chrono::system_clock::now();
	
	// Dump alignment info 
	// @GGGG: this should be fine parallel now
	#pragma omp parallel
	{
	    #pragma omp for
		for (uint64_t i = 0; i < npairs; ++i)
		{
			// Only keep alignments that meet BELLA criteria
			bool passed = false;

			dibella::CommonKmers *cks = std::get<2>(mattuples[lids[i]]);
			PostAlignDecision(ai[i], passed, ratioScoreOverlap, cks->overhang, cks->overhangT, cks->overlap, noAlign);

			if (passed)
			{
				// GGGG: store updated seed start/end position in the CommonKmers pairs (the semantics of these pairs change wrt the original semantics but that's okay)
				cks->first.first   = ai[i].begSeedV; 	// start on ver sequence
				cks->second.first  = ai[i].begSeedH;	    // start on hor sequence

				cks->first.second  = ai[i].endSeedV; 		// end on ver sequence
				cks->second.second = ai[i].endSeedH;		// end on hor sequence

				cks->lenv 	= ai[i].seq_v_length;
				cks->lenh 	= ai[i].seq_h_length;

				cks->score  = ai[i].xscore;
				cks->passed = passed;	// keep this
			}
		}
	}

	auto end_time = std::chrono::system_clock::now();
  	add_time("XA:StringOp",
			 (ms_t(end_time - start_time)).count());

	delete [] ai;

	return;
}
