package writer

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// ParsedChannel mirrors what channeldb.c::read_channeldb pulls back
// off disk for one channel. Used by tests to verify our writer's
// output round-trips through the same logic the ircd will use.
type ParsedChannel struct {
	Name         string
	CreationTime int64
	Topic        string
	TopicNick    string
	TopicTime    int64
	Modes1       string // "+nt"
	Modes2       string // params, space-separated
	ModeLock     string
	RegisteredBy string
	RegisteredAt int64
	Banlist      []ParsedListEntry
	Excepts      []ParsedListEntry
	Invex        []ParsedListEntry
}

type ParsedListEntry struct {
	BanStr string
	Who    string
	When   int64
}

// ParseChannelDB reads a v101 channel.db buffer back into structs
// using the same framing as channeldb.c::read_channeldb. Returns the
// schema version and the channel list.
func ParseChannelDB(data []byte) (uint32, []ParsedChannel, error) {
	r := &cdbReader{data: data}
	version, err := r.uint32()
	if err != nil {
		return 0, nil, err
	}
	count, err := r.uint64()
	if err != nil {
		return version, nil, err
	}
	out := make([]ParsedChannel, 0, count)
	for i := uint64(0); i < count; i++ {
		ch, err := readOneChannel(r, version)
		if err != nil {
			return version, out, fmt.Errorf("channel %d: %w", i, err)
		}
		out = append(out, ch)
	}
	return version, out, nil
}

func readOneChannel(r *cdbReader, version uint32) (ParsedChannel, error) {
	var ch ParsedChannel
	magic, err := r.uint32()
	if err != nil {
		return ch, err
	}
	if magic != magicEntryStart {
		return ch, fmt.Errorf("bad magic_start 0x%x", magic)
	}
	ch.Name, _ = r.str()
	ct, _ := r.uint64()
	ch.CreationTime = int64(ct)
	ch.Topic, _ = r.str()
	ch.TopicNick, _ = r.str()
	tt, _ := r.uint64()
	ch.TopicTime = int64(tt)
	ch.Modes1, _ = r.str()
	ch.Modes2, _ = r.str()
	ch.ModeLock, _ = r.str()
	if version >= 101 {
		ch.RegisteredBy, _ = r.str()
		ra, _ := r.uint64()
		ch.RegisteredAt = int64(ra)
	}
	ch.Banlist, err = readListMode(r)
	if err != nil {
		return ch, fmt.Errorf("ban list: %w", err)
	}
	ch.Excepts, err = readListMode(r)
	if err != nil {
		return ch, fmt.Errorf("except list: %w", err)
	}
	ch.Invex, err = readListMode(r)
	if err != nil {
		return ch, fmt.Errorf("invex list: %w", err)
	}
	endMagic, err := r.uint32()
	if err != nil {
		return ch, err
	}
	if endMagic != magicEntryEnd {
		return ch, fmt.Errorf("bad magic_end 0x%x", endMagic)
	}
	return ch, nil
}

func readListMode(r *cdbReader) ([]ParsedListEntry, error) {
	n, err := r.uint32()
	if err != nil {
		return nil, err
	}
	out := make([]ParsedListEntry, 0, n)
	for i := uint32(0); i < n; i++ {
		var e ParsedListEntry
		e.BanStr, _ = r.str()
		e.Who, _ = r.str()
		w, _ := r.uint64()
		e.When = int64(w)
		out = append(out, e)
	}
	return out, nil
}

type cdbReader struct {
	data []byte
	off  int
}

func (r *cdbReader) need(n int) error {
	if r.off+n > len(r.data) {
		return io.ErrUnexpectedEOF
	}
	return nil
}

func (r *cdbReader) uint16() (uint16, error) {
	if err := r.need(2); err != nil {
		return 0, err
	}
	v := binary.LittleEndian.Uint16(r.data[r.off:])
	r.off += 2
	return v, nil
}

func (r *cdbReader) uint32() (uint32, error) {
	if err := r.need(4); err != nil {
		return 0, err
	}
	v := binary.LittleEndian.Uint32(r.data[r.off:])
	r.off += 4
	return v, nil
}

func (r *cdbReader) uint64() (uint64, error) {
	if err := r.need(8); err != nil {
		return 0, err
	}
	v := binary.LittleEndian.Uint64(r.data[r.off:])
	r.off += 8
	return v, nil
}

func (r *cdbReader) str() (string, error) {
	l, err := r.uint16()
	if err != nil {
		return "", err
	}
	if l == stringNullSentinel {
		return "", nil
	}
	if int(l) == 0 {
		return "", nil
	}
	if err := r.need(int(l)); err != nil {
		return "", err
	}
	s := string(r.data[r.off : r.off+int(l)])
	r.off += int(l)
	return s, nil
}

var _ = errors.New // keep imports happy if package shrinks
