import { Button } from "@/components/ui/button";

export function ArtifactPageControls({ label, pageNumber, count, nextCursor, isLoading, onPrevious, onNext }: {
  label: string;
  pageNumber: number;
  count: number;
  nextCursor: string | null;
  isLoading: boolean;
  onPrevious: () => void;
  onNext: (cursor: string) => void;
}) {
  return (
    <nav aria-label={`${label} pages`} className="flex flex-wrap items-center justify-between gap-3 py-3 text-xs">
      <span aria-live="polite">{isLoading ? `Loading ${label.toLowerCase()}...` : `${label}, page ${pageNumber}: ${count} records${nextCursor ? "; more available" : "; end of list"}`}</span>
      <div className="flex gap-2">
        <Button size="sm" variant="outline" disabled={isLoading || pageNumber === 1} onClick={onPrevious}>Previous {label.toLowerCase()}</Button>
        <Button size="sm" variant="outline" disabled={isLoading || nextCursor === null} onClick={() => { if (nextCursor) onNext(nextCursor); }}>Next {label.toLowerCase()}</Button>
      </div>
    </nav>
  );
}
