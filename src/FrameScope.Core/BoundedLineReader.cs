using System.Text;

namespace FrameScope.Core;

public sealed class BoundedLineReader(TextReader reader, int maximumCharacters = IntelJsonDecoder.MaximumLineCharacters)
{
    private readonly char[] _buffer = new char[4096];
    private int _offset, _count;
    public async Task<string?> ReadLineAsync(CancellationToken cancellationToken = default)
    {
        if (maximumCharacters <= 0) throw new ArgumentOutOfRangeException(nameof(maximumCharacters));
        var builder = new StringBuilder(Math.Min(8192, maximumCharacters));
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_offset == _count)
            {
                _count = await reader.ReadAsync(_buffer.AsMemory(), cancellationToken).ConfigureAwait(false); _offset = 0;
                if (_count == 0) return builder.Length == 0 ? null : builder.ToString();
            }
            var end = System.Array.IndexOf(_buffer, '\n', _offset, _count - _offset);
            var length = (end < 0 ? _count : end) - _offset;
            if (builder.Length + length > maximumCharacters) throw new InvalidDataException("Строка helper превышает ограничение размера.");
            builder.Append(_buffer, _offset, length); _offset += length;
            if (end < 0) continue;
            _offset++;
            if (builder.Length > 0 && builder[^1] == '\r') builder.Length--;
            return builder.ToString();
        }
    }
}
