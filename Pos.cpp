#include "gb-include.h"

#include "Pos.h"
#include "Words.h"
#include "Sections.h"

Pos::Pos() {
	m_buf = NULL;
	m_needsFree = false;
}

Pos::~Pos () {
	reset();
}

void Pos::reset() {
	if ( m_buf && m_needsFree )
		mfree ( m_buf , m_bufSize , "Pos" );
	m_buf = NULL;
}

static bool inTag( nodeid_t tagId, nodeid_t expectedTagId, int *count ) {
	if ( !count ) {
		return false;
	}

	if ( tagId == expectedTagId ) {
		++( *count );
	}

	if ( *count ) {
		// back tag
		if ( ( tagId & BACKBITCOMP ) == expectedTagId ) {
			--( *count );
		}
	}

	return ( *count > 0 );
}

int32_t Pos::filter( Words *words, int32_t a, int32_t b, bool addEllipsis, char *f, char *fend ) {
	nodeid_t *tids = words->getTagIds(); // m_tagIds;

	// save start point for filtering
	char *fstart = f;

	// -1 is the default value
	if ( b == -1 ) {
		b = words->getNumWords();
	}

	bool trunc = false;

	static const int32_t maxCharSize = 4; // we are utf8

	char* prevChar = NULL;

	char* lastBreak = NULL;
	char* lastBreakPrevChar = NULL; // store char before space

	// flag for stopping back-to-back spaces. only count those as one char.
	bool lastSpace = false;

	int inBadTags = 0;
	int capCount = 0;

	int dotCount = 0; // store last encountered total consecutive dots
	char* dotPrevChar = NULL; // store char before dot which is not a space

	for ( int32_t i = a ; i < b ; ++i ) {
		if (trunc) {
			break;
		}

		// is tag?
		if ( tids && tids[i] ) {
			// let's not get from bad tags
			if ( inTag( tids[i], TAG_STYLE, &inBadTags ) ) {
				continue;
			}

			if ( inTag( tids[i], TAG_SCRIPT, &inBadTags ) ) {
				continue;
			}

			// if not breaking, does nothing
			if ( !g_nodes[tids[i] & 0x7f].m_isBreaking ) {
				continue;
			}

			// list tag? <li>
			if ( tids[i] == TAG_LI ) {
				if ( ( fend - f > maxCharSize ) ) {
					*f++ = '*';

					// counted as caps because we're detecting all caps for a sentence
					++capCount;
				} else {
					trunc = true;
				}

				lastSpace = false;
				continue;
			}

			// if had a previous breaking tag and no non-tag
			// word after it, do not count back-to-back spaces
			if ( lastSpace ) {
				continue;
			}

			// if had a br tag count it as a '.'
			if ( tids[i] ) { // <br>
				if ( f != fstart ) {
					if ( ( fend - f > 2 * maxCharSize ) ) {
						if ( is_ascii(*prevChar) && (*prevChar != '.') ) {
							*f++ = '.';

							// counted as caps because we're detecting all caps for a sentence
							++capCount;
						}

						*f++ = ' ';
						++capCount;
					} else {
						trunc = true;
					}
				}

				lastSpace = true;

				continue;
			}

			if ( ( fend - f > maxCharSize ) ) {
				*f++ = ' ';
			} else {
				trunc = true;
			}

			// do not allow back-to-back spaces
			lastSpace = true;

			continue;
		}

		// scan through all chars discounting back-to-back spaces
		char *pend = words->getWord(i) + words->getWordLen(i);
		unsigned char cs = 0;

		char *p    = NULL ;

		// assume filters out to the same # of chars
		for ( p = words->getWord(i); p < pend; p += cs ) {
			// get size
			cs = getUtf8CharSize(p);

			// skip unwanted character
			if ( isUtf8UnwantedSymbols( p ) ) {
				continue;
			}

			// do not count space if one before
			if ( is_wspace_utf8 (p) ) {
				if ( lastSpace ) {
					continue;
				}

				lastSpace = true;

				if ( fend - f > 1 ) {
					lastBreakPrevChar = prevChar;

					// don't store lastBreak if we have less than ellipsis length ' ...'
					if ( fend - f > 4 ) {
						lastBreak = f;
					}

					*f++ = ' ';

					// counted as caps because we're detecting all caps for a sentence
					++capCount;

					dotCount = 0;

					// we don't store space as dotPreviousChar because we want to strip ' ...' as well
				} else {
					trunc = true;
				}

				continue;
			}

			if ( fend - f > cs ) {
				prevChar = f;

				if ( cs == 1 ) {
					// we only do it for ascii to avoid catering for different rules in different languages
					// https://en.wikipedia.org/wiki/Letter_case#Exceptional_letters_and_digraphs
					// eg:
					//   The Greek upper-case letter "Σ" has two different lower-case forms:
					//     "ς" in word-final position and "σ" elsewhere
					if ( !is_alpha_a( *p ) || is_upper_a( *p ) ) {
						// non-alpha is counted as caps as well because we're detecting all caps for a sentence
						// and comma/quotes/etc. is included
						++capCount;
					}

					// some sites try to be smart and truncate for us, let's remove that
					// if if there are no space between dots and letter
					if ( *p == '.' ) {
						++dotCount;
					} else {
						dotCount = 0;
						dotPrevChar = f;
					}

					*f++ = *p;
				} else {
					dotCount = 0;
					dotPrevChar = f;

					gbmemcpy( f, p, cs );
					f += cs;
				}
			} else {
				trunc = true;
			}

			lastSpace = false;
		}
	}

	/// @todo ALC simplify logic/break into smaller functions

	/// @todo ALC configurable minCapCount so we can tweak this as needed
	const int minCapCount = 5;

	// only capitalize first letter in a word for a sentence with all caps
	if ( capCount > minCapCount && capCount == ( f - fstart ) ) {
		bool isFirstLetter = true;

		unsigned char cs = 0;
		for ( char *c = fstart; c < f; c += cs ) {
			cs = getUtf8CharSize(c);

			bool isAlpha = is_alpha_utf8( c );

			if ( isAlpha ) {
				if (isFirstLetter) {
					isFirstLetter = false;
					continue;
				}
			} else {
				isFirstLetter = true;
				continue;
			}

			if ( !isFirstLetter ) {
				to_lower_utf8(c, c);
			}
		}
	}

	/// @todo ALC configurable minRemoveEllipsisLen so we can tweak this as needed
	const int minRemoveEllipsisLen = 120;

	// let's remove ellipsis (...) at the end
	if ( (f - fstart) >= minRemoveEllipsisLen && dotCount == 3 ) {
		if ( is_ascii3( *dotPrevChar ) ) {
			switch ( *dotPrevChar ) {
				case ',':
					trunc = true;
					lastBreak = dotPrevChar + 1;
					break;
				case '!':
				case '.':
					trunc = false;
					f = dotPrevChar + 1;
					break;
				case ' ':
					trunc = false;

					if ( lastBreak ) {
						f = lastBreak;
					}
					break;
				default:
					trunc = true;

					if ( lastBreakPrevChar ) {
						if ( is_ascii( *( lastBreakPrevChar ) ) ) {
							switch ( *( lastBreakPrevChar ) ) {
								case '!':
								case '.':
									trunc = false;

									if (lastBreak) {
										f = lastBreak;
									}
									break;
								default:
									break;
							}
						}
					}
					break;
			}
		}
	}

	if ( trunc ) {
		if ( lastBreak == NULL ) {
			return 0;
		}

		f = lastBreak;

		/// @todo ALC we should cater ellipsis for different languages
		if ( addEllipsis ) {
			if ( (fend - f) > 4 ) {
				gbmemcpy ( f , " ..." , 4 );
				f += 4;
			}
		}
	}

	// NULL terminate f
	*f = '\0';

	return (f - fstart);
}

bool Pos::set( Words *words, int32_t a, int32_t b ) {
	// free m_buf in case this is a second call
	reset();

	int32_t nw = words->getNumWords();
	int32_t *wlens = words->m_wordLens;
	nodeid_t *tids = words->getTagIds(); // m_tagIds;
	char **wp = words->m_words;

	// -1 is the default value
	if ( b == -1 ) {
		b = nw;
	}

	// alloc array if need to
	int32_t need = (nw+1) * 4;

	// do not destroy m_pos/m_numWords if only filtering into a buffer
	m_needsFree = false;

	m_buf = m_localBuf;
	if ( need > POS_LOCALBUFSIZE ) {
		m_buf = (char *)mmalloc(need,"Pos");
		m_needsFree = true;
	}

	// bail on error
	if ( ! m_buf ) {
		return false;
	}

	m_bufSize = need;
	m_pos      = (int32_t *)m_buf;

	// this is the CHARACTER count.
	int32_t pos = 0;

	// flag for stopping back-to-back spaces. only count those as one char.
	bool lastSpace = false;

	for ( int32_t i = a ; i < b ; i++ ) {
		// set pos for the ith word to "pos"
		m_pos[i] = pos;

		// is tag?
		if ( tids && tids[i] ) {
			// if not breaking, does nothing
			if ( !g_nodes[tids[i] & 0x7f].m_isBreaking ) {
				continue;
			}

			// list tag? <li>
			if ( tids[i] == TAG_LI ) {
				++pos;
				lastSpace = false;
				continue;
			}

			// if had a previous breaking tag and no non-tag
			// word after it, do not count back-to-back spaces
			if ( lastSpace ) {
				continue;
			}

			// if had a br tag count it as a '. '
			if ( tids[i] ) { // <br>
				pos += 2;
				lastSpace = true;

				continue;
			}

			// count as a single space
			pos++;

			// do not allow back-to-back spaces
			lastSpace = true;

			continue;
		}

		// scan through all chars discounting back-to-back spaces
		char *pend = wp[i] + wlens[i];
		unsigned char cs = 0;

		// assume filters out to the same # of chars
		for ( char *p = wp[i]; p < pend; p += cs ) {
			// get size
			cs = getUtf8CharSize(p);

			// do not count space if one before
			if ( is_wspace_utf8 (p) ) {
				if ( lastSpace ) {
					continue;
				}

				lastSpace = true;

				++pos;
				continue;
			}

			++pos;
			lastSpace = false;
		}
	}

	// set pos for the END of the last word here
	m_pos[nw] = pos;

	return true;
}
