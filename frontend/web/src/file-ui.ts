import {
    enqueue,
    type FileHeader,
    getCurrentDocId,
    listDocuments,
    save,
} from "./storage.ts";

export interface FileUIActions {
    openDocument: (id: string) => Promise<void>;
    deleteDocument: (id: string) => Promise<void>;
}

export function createFileUI(actions: FileUIActions): {
    show: () => void;
    hide: () => void;
} {
    const modal = document.querySelector<HTMLDivElement>("#file-ui")!;
    const list = document.querySelector<HTMLDivElement>("#file-ui-list")!;
    const closeButton =
        document.querySelector<HTMLButtonElement>("#close-file-ui")!;

    function show() {
        if (!modal.hidden) return;
        modal.hidden = false;
        refresh();
    }

    function hide() {
        modal.hidden = true;
    }

    function refresh() {
        return enqueue(async () => {
            await save();
            render(await listDocuments());
        });
    }

    function render(files: FileHeader[]) {
        files.sort(
            (a, b) => b.lastModified.getTime() - a.lastModified.getTime(),
        );
        const currentId = getCurrentDocId();
        list.replaceChildren(
            ...files.map((f) => {
                const name = f.readable
                    ? f.title || "Untitled"
                    : "(unreadable)";
                const openButton = document.createElement("button");
                openButton.textContent = `${name} - ${f.lastModified.toLocaleString()}`;
                openButton.disabled = !f.readable || f.id === currentId;
                openButton.addEventListener("click", () => {
                    hide();
                    actions.openDocument(f.id);
                });

                const deleteButton = document.createElement("button");
                deleteButton.textContent = "Delete";
                deleteButton.addEventListener("click", () => {
                    if (!confirm(`Delete "${name}"?`)) return;
                    actions.deleteDocument(f.id);
                    refresh();
                });

                const row = document.createElement("div");
                row.append(openButton, deleteButton);
                return row;
            }),
        );
    }

    closeButton.addEventListener("click", hide);
    return { show, hide };
}
