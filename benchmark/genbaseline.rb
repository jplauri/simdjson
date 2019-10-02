class ChunkWriter
    def initialize(base_filename="baseline-", file_size=640*1024, block_size=64)
        puts "#{file_size}"
        @@base_filename = base_filename
        @@file_size = file_size
        @@block_size = block_size
    end

    def prepare_chunk(chunks, include_newline)
        Array(chunks).map do |chunk|
            "#{chunk}#{' '*(@@block_size-chunk.bytesize-1)}#{include_newline ? "\n" : " "}"
        end.join("")
    end

    def write_file(filename, start_chunks, repeat_chunks, end_chunks, include_newline=true)
        filename = "#{@@base_filename}#{filename}"
        puts "Writing #{filename} ..."
        start_chunk = prepare_chunk(start_chunks, include_newline)
        repeat_chunk = prepare_chunk(repeat_chunks, include_newline)
        end_chunk = prepare_chunk(end_chunks, include_newline)
        pos = 0
        File.open(filename, "w") do |file|
            file.write(start_chunk)
            pos += start_chunk.bytesize
            while pos < (@@file_size-(end_chunk.bytesize))
                file.write(repeat_chunk)
                pos += repeat_chunk.bytesize
            end
            file.write(end_chunk)
            pos += end_chunk.bytesize
        end
        raise "OMG wrong length #{pos} (should be #{@@file_size}" if pos != @@file_size
        raise "OMG wrong file size #{File.size(filename)} (should be #{@@file_size})" if File.size(filename) != @@file_size
    end

    def write_file_thirds(filename, start_chunks, repeat_chunks1, repeat_chunks2, end_chunks, include_newline=true)
        filename = "#{@@base_filename}#{filename}"
        puts "Writing #{filename} ..."
        start_chunk = prepare_chunk(start_chunks, include_newline)
        repeat_chunk1 = prepare_chunk(repeat_chunks1, include_newline)
        repeat_chunk2 = prepare_chunk(repeat_chunks2, include_newline)
        end_chunk = prepare_chunk(end_chunks, include_newline)
        pos = 0
        one_third_repeat_length = (@@file_size - start_chunk.bytesize - end_chunk.bytesize) / 3
        File.open(filename, "w") do |file|
            file.write(start_chunk)
            pos += start_chunk.bytesize
            while pos < (start_chunk.bytesize+one_third_repeat_length)
                file.write(repeat_chunk1)
                pos += repeat_chunk1.bytesize
            end
            while pos < (@@file_size-(end_chunk.bytesize))
                file.write(repeat_chunk2)
                pos += repeat_chunk2.bytesize
            end
            file.write(end_chunk)
            pos += end_chunk.bytesize
        end
        raise "OMG wrong length #{pos} (should be #{@@file_size}" if pos != @@file_size
        raise "OMG wrong file size #{File.size(filename)} (should be #{@@file_size})" if File.size(filename) != @@file_size
    end
end


w = ChunkWriter.new(*ARGV)
w.write_file "structurals-0.json",  '0', '', ''
w.write_file "structurals-1.json",  '[', ['0' , ','], ['{', '}', ']']
w.write_file "structurals-2.json",  '[0', ',0', [',{', '}]']
w.write_file "structurals-3.json",  '[{}', ',{}', ',0]'
w.write_file "structurals-4.json",  '[0,0', ',0,0', ',{}]'
w.write_file "structurals-5.json",  '[0,{}', ',0,{}', ',0,0]'
w.write_file "structurals-6.json",  '[0,0,0', ',0,0,0', ',0,{}]'
w.write_file "structurals-7.json",  '[0,0,{}', ',0,0,{}', ',0,0,0]'
w.write_file "structurals-8.json",  '[0,0,0,0', ',0,0,0,0', ',0,0,{}]'
w.write_file "structurals-9.json",  '[0,0,0,{}', ',0,0,0,{}', ',0,0,0,0]'
w.write_file "structurals-10.json", '[0,0,0,0,0', ',0,0,0,0,0', ',0,0,0,{}]'
w.write_file "structurals-11.json", '[0,0,0,0,{}', ',0,0,0,0,{}', ',0,0,0,0,0]'
w.write_file "structurals-12.json", '[0,0,0,0,0,0', ',0,0,0,0,0,0', ',0,0,0,0,{}]'
w.write_file "structurals-13.json", '[0,0,0,0,0,{}', ',0,0,0,0,0,{}', ',0,0,0,0,0,0]'
w.write_file "structurals-14.json", '[0,0,0,0,0,0,0', ',0,0,0,0,0,0,0', ',0,0,0,0,0,{}]'
w.write_file "structurals-15.json", '[0,0,0,0,0,0,{}', ',0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0]'
w.write_file "structurals-16.json", '[0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,{}]'
w.write_file "structurals-17.json", '[0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,0]'
w.write_file "structurals-18.json", '[0,0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,0,{}]'
w.write_file "structurals-19.json", '[0,0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,0,0]'
w.write_file "structurals-20.json", '[0,0,0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,0,0,{}]'
w.write_file "structurals-21.json", '[0,0,0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,0,0,0]'
w.write_file "structurals-22.json", '[0,0,0,0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,0,0,0,0,0', ',0,0,0,0,0,0,0,0,0,{}]'
w.write_file "structurals-23.json", '[0,0,0,0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,0,0,0,{}', ',0,0,0,0,0,0,0,0,0,0,0]'
w.write_file "utf-8.json",          '["€"', ',"€"', ',"€"]'
w.write_file_thirds "one-third-structurals-0.json",  '[0', '',                  ',0', ']'
w.write_file_thirds "one-third-structurals-9.json",  '[0', ',0,0,0,{}',         ',0', ']'
w.write_file_thirds "one-third-structurals-17.json", '[0', ',0,0,0,0,0,0,0,{}', ',0', ']'
w.write_file_thirds "one-third-utf-8.json",          '[0', ',"€"',              ',0', ']'
w.write_file "one-third-structurals-0-flip.json",    '[0', ['',                  ',0', '',                  ',0', ',0', '',                  ',0', ',0', ',0', '',                  ',0', ',0'], [ '',                  ',0', ']' ]
w.write_file "one-third-structurals-9-flip.json",    '[0', [',0,0,0,{}',         ',0', ',0,0,0,{}',         ',0', ',0', ',0,0,0,{}',         ',0', ',0', ',0', ',0,0,0,{}',         ',0', ',0'], [ ',0,0,0,{}',         ',0', ']' ]
w.write_file "one-third-structurals-17-flip.json",   '[0', [',0,0,0,0,0,0,0,{}', ',0', ',0,0,0,0,0,0,0,{}', ',0', ',0', ',0,0,0,0,0,0,0,{}', ',0', ',0', ',0', ',0,0,0,0,0,0,0,{}', ',0', ',0'], [ ',0,0,0,0,0,0,0,{}', ',0', ']' ]
w.write_file "one-third-utf-8-flip.json",            '[0', [',"€"',              ',0', ',"€"',              ',0', ',0', ',"€"',              ',0', ',0', ',0', ',"€"',              ',0', ',0'], [ ',"€"',              ',0', ']' ]
